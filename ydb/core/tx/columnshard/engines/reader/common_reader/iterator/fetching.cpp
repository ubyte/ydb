#include "constructor.h"
#include "default_fetching.h"
#include "fetch_steps.h"
#include "fetching.h"
#include "source.h"
#include "sub_columns_fetching.h"

#include <ydb/core/kqp/runtime/scheduler/new/kqp_schedulable_actor.h>
#include <ydb/core/tx/columnshard/blobs_reader/actor.h>

#include <util/string/builder.h>
#include <yql/essentials/minikql/mkql_terminator.h>

namespace NKikimr::NOlap::NReader::NCommon {

bool TStepAction::DoApply(IDataReader& owner) const {
    AFL_VERIFY(FinishedFlag);
    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD_SCAN)("event", "apply");
    Source->StartSyncSection();
    Source->OnSourceFetchingFinishedSafe(owner, Source);
    return true;
}

TConclusion<bool> TStepAction::DoExecuteImpl() {
    FOR_DEBUG_LOG(NKikimrServices::COLUMNSHARD_SCAN_EVLOG, Source->AddEvent("step_action"));
    if (Source->GetContext()->IsAborted()) {
        AFL_VERIFY(!FinishedFlag);
        FinishedFlag = true;
        return true;
    }
    auto executeResult = Cursor.Execute(Source);
    if (executeResult.IsFail()) {
        AFL_VERIFY(!FinishedFlag);
        FinishedFlag = true;
        return executeResult;
    }
    if (*executeResult) {
        AFL_VERIFY(!FinishedFlag);
        FinishedFlag = true;
    }
    return FinishedFlag;
}

TStepAction::TStepAction(const std::shared_ptr<IDataSource>& source, TFetchingScriptCursor&& cursor, const NActors::TActorId& ownerActorId,
    const bool changeSyncSection)
    : TBase(ownerActorId, source->GetContext()->GetCommonContext()->GetCounters().GetAssembleTasksGuard())
    , Source(source)
    , Cursor(std::move(cursor)) {
    if (changeSyncSection) {
        Source->StartAsyncSection();
    } else {
        Source->CheckAsyncSection();
    }
}

TConclusion<bool> TFetchingScriptCursor::Execute(const std::shared_ptr<IDataSource>& source) {
    AFL_VERIFY(source);
    NMiniKQL::TThrowingBindTerminator bind;
    Script->OnExecute();
    AFL_VERIFY(!Script->IsFinished(CurrentStepIdx));
    while (!Script->IsFinished(CurrentStepIdx)) {
        if (source->HasStageData() && source->GetStageData().IsEmptyWithData()) {
            source->OnEmptyStageData(source);
            break;
        } else if (source->HasStageResult() && source->GetStageResult().IsEmpty()) {
            break;
        }
        auto step = Script->GetStep(CurrentStepIdx);
        std::optional<TMemoryProfileGuard> mGuard;
        if (IS_DEBUG_LOG_ENABLED(NKikimrServices::TX_COLUMNSHARD_SCAN_MEMORY)) {
            mGuard.emplace("SCAN_PROFILE::FETCHING::" + step->GetName() + "::" + Script->GetBranchName());
        }
        AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD_SCAN)("scan_step", step->DebugString())("scan_step_idx", CurrentStepIdx)(
            "source_id", source->GetSourceId());

        auto& schedulableTask = source->GetContext()->GetCommonContext()->GetSchedulableTask();
        if (schedulableTask) {
            schedulableTask->IncreaseExtraUsage();
        }

        const TMonotonic startInstant = TMonotonic::Now();
        const TConclusion<bool> resultStep = step->ExecuteInplace(source, *this);
        const auto executionTime = TMonotonic::Now() - startInstant;
        source->GetContext()->GetCommonContext()->GetCounters().AddExecutionDuration(executionTime);

        if (schedulableTask) {
            schedulableTask->DecreaseExtraUsage(executionTime);
        }

        FlushDuration(executionTime);
        if (!resultStep) {
            return resultStep;
        }
        if (!*resultStep) {
            return false;
        }
        ++CurrentStepIdx;
    }
    FOR_DEBUG_LOG(NKikimrServices::COLUMNSHARD_SCAN_EVLOG, source->AddEvent("fcursor"));
    return true;
}

TString TFetchingScript::DebugString() const {
    TStringBuilder sb;
    TStringBuilder sbBranch;
    for (auto&& i : Steps) {
        sbBranch << "{" << i->DebugString() << "};";
    }
    if (!sbBranch) {
        return "";
    }
    sb << "{branch:" << BranchName << ";steps:[" << sbBranch << "]}";
    return sb;
}

TString TFetchingScript::ProfileDebugString() const {
    TStringBuilder sb;
    TStringBuilder sbBranch;
    for (auto&& i : Steps) {
        if (i->GetSumDuration() > TDuration::MilliSeconds(10)) {
            sbBranch << "{" << i->DebugString(true) << "};";
        }
    }
    if (!sbBranch) {
        return "";
    }
    sb << "{branch:" << BranchName << ";";
    if (AtomicGet(FinishInstant) && AtomicGet(StartInstant)) {
        sb << "duration:" << AtomicGet(FinishInstant) - AtomicGet(StartInstant) << ";";
    }

    sb << "steps_10Ms:[" << sbBranch << "]}";
    return sb;
}

void TFetchingScriptBuilder::AddAllocation(
    const std::set<ui32>& entityIds, const NArrow::NSSA::IMemoryCalculationPolicy::EStage stage, const EMemType mType) {
    if (Steps.size() == 0) {
        AddStep(std::make_shared<TAllocateMemoryStep>(entityIds, mType, stage));
    } else {
        std::optional<ui32> addIndex;
        for (i32 i = Steps.size() - 1; i >= 0; --i) {
            if (auto allocation = std::dynamic_pointer_cast<TAllocateMemoryStep>(Steps[i])) {
                if (allocation->GetStage() == stage) {
                    allocation->AddAllocation(entityIds, mType);
                    return;
                } else {
                    addIndex = i + 1;
                }
                break;
            } else if (std::dynamic_pointer_cast<TAssemblerStep>(Steps[i])) {
                continue;
            } else if (std::dynamic_pointer_cast<TColumnBlobsFetchingStep>(Steps[i])) {
                continue;
            } else {
                addIndex = i + 1;
                break;
            }
        }
        AFL_VERIFY(addIndex);
        InsertStep<TAllocateMemoryStep>(*addIndex, entityIds, mType, stage);
    }
}

TString IFetchingStep::DebugString(const bool stats) const {
    TStringBuilder sb;
    sb << "name=" << Name;
    if (stats) {
        sb << ";duration=" << GetSumDuration() << ";"
           << "size=" << 1e-9 * GetSumSize();
    }
    sb << ";details={" << DoDebugString() << "};";
    return sb;
}

TFetchingScriptBuilder::TFetchingScriptBuilder(const TSpecialReadContext& context)
    : TFetchingScriptBuilder(context.GetReadMetadata()->GetResultSchema(), context.GetMergeColumns()) {
}

void TFetchingScriptBuilder::AddFetchingStep(const TColumnsSetIds& columns, const NArrow::NSSA::IMemoryCalculationPolicy::EStage stage) {
    auto actualColumns = columns - AddedFetchingColumns;
    AddedFetchingColumns += columns;
    if (actualColumns.IsEmpty()) {
        return;
    }
    if (Steps.size() && std::dynamic_pointer_cast<TColumnBlobsFetchingStep>(Steps.back())) {
        TColumnsSetIds fetchingColumns = actualColumns + std::dynamic_pointer_cast<TColumnBlobsFetchingStep>(Steps.back())->GetColumns();
        Steps.pop_back();
        AddAllocation(actualColumns.GetColumnIds(), stage, EMemType::Blob);
        AddStep(std::make_shared<TColumnBlobsFetchingStep>(fetchingColumns));
    } else {
        AddAllocation(actualColumns.GetColumnIds(), stage, EMemType::Blob);
        AddStep(std::make_shared<TColumnBlobsFetchingStep>(actualColumns));
    }
}

void TFetchingScriptBuilder::AddAssembleStep(
    const TColumnsSetIds& columns, const TString& purposeId, const NArrow::NSSA::IMemoryCalculationPolicy::EStage stage, const bool sequential) {
    auto actualColumns = columns - AddedAssembleColumns;
    AddedAssembleColumns += columns;
    if (actualColumns.IsEmpty()) {
        return;
    }
    auto actualSet = std::make_shared<TColumnsSet>(actualColumns.GetColumnIds(), FullSchema);
    if (sequential) {
        const auto notSequentialColumnIds = GuaranteeNotOptional->Intersect(*actualSet);
        if (notSequentialColumnIds.size()) {
            AddAllocation(notSequentialColumnIds, stage, EMemType::Raw);
            std::shared_ptr<TColumnsSet> cross = actualSet->BuildSamePtr(notSequentialColumnIds);
            AddStep(std::make_shared<TAssemblerStep>(cross, purposeId));
            *actualSet = *actualSet - *cross;
        }
        if (!actualSet->IsEmpty()) {
            AddAllocation(actualSet->GetColumnIds(), stage, EMemType::RawSequential);
            AddStep(std::make_shared<TOptionalAssemblerStep>(actualSet, purposeId));
        }
    } else {
        AddAllocation(actualColumns.GetColumnIds(), stage, EMemType::Raw);
        AddStep(std::make_shared<TAssemblerStep>(actualSet, purposeId));
    }
}

TConclusion<bool> TProgramStep::DoExecuteInplace(const std::shared_ptr<IDataSource>& source, const TFetchingScriptCursor& step) const {
    const bool started = !source->GetExecutionContext().HasProgramIterator();
    if (!source->GetExecutionContext().HasProgramIterator()) {
        source->MutableExecutionContext().Start(source, Program, step);
    }
    auto iterator = source->GetExecutionContext().GetProgramIteratorVerified();
    const auto& resources = source->GetStageData().GetTable();
    if (!started) {
        iterator->Next();
        source->MutableExecutionContext().OnFinishProgramStepExecution();
    }
    for (; iterator->IsValid();) {
        {
            auto conclusion = iterator->Next();
            if (conclusion.IsFail()) {
                return conclusion;
            }
        }
        if (!source->GetExecutionContext().GetExecutionVisitorVerified()->GetExecutionNode()) {
            if (iterator->IsValid()) {
                GetSignals(iterator->GetCurrentNodeId())->OnSkipGraphNode(source->GetRecordsCount());
                source->GetContext()->GetCommonContext()->GetCounters().OnSkipGraphNode(iterator->GetCurrentNode().GetIdentifier());
            }
            continue;
        }
        AFL_VERIFY(source->GetExecutionContext().GetExecutionVisitorVerified()->GetExecutionNode()->GetIdentifier() == iterator->GetCurrentNodeId());
        source->MutableExecutionContext().OnStartProgramStepExecution(iterator->GetCurrentNodeId(), GetSignals(iterator->GetCurrentNodeId()));
        auto signals = GetSignals(iterator->GetCurrentNodeId());
        const TMonotonic start = TMonotonic::Now();
        auto conclusion = source->GetExecutionContext().GetExecutionVisitorVerified()->Execute();
        source->GetContext()->GetCommonContext()->GetCounters().AddExecutionDuration(TMonotonic::Now() - start);
        signals->AddExecutionDuration(TMonotonic::Now() - start);
        if (conclusion.IsFail()) {
            source->MutableExecutionContext().OnFailedProgramStepExecution();
            return conclusion;
        } else if (*conclusion == NArrow::NSSA::IResourceProcessor::EExecutionResult::InBackground) {
            return false;
        }
        source->MutableExecutionContext().OnFinishProgramStepExecution();
        GetSignals(iterator->GetCurrentNodeId())->OnExecuteGraphNode(source->GetRecordsCount());
        source->GetContext()->GetCommonContext()->GetCounters().OnExecuteGraphNode(iterator->GetCurrentNode().GetIdentifier());
        if (resources->GetRecordsCountActualOptional() == 0) {
            resources->Clear();
            break;
        }
    }
    FOR_DEBUG_LOG(NKikimrServices::COLUMNSHARD_SCAN_EVLOG, source->AddEvent("fgraph"));
    AFL_DEBUG(NKikimrServices::SSA_GRAPH_EXECUTION)(
        "graph_constructed", Program->DebugDOT(source->GetExecutionContext().GetExecutionVisitorVerified()->GetExecutedIds()));

    return true;
}

const std::shared_ptr<TFetchingStepSignals>& TProgramStep::GetSignals(const ui32 nodeId) const {
    auto it = Signals.find(nodeId);
    AFL_VERIFY(it != Signals.end())("node_id", nodeId);
    return it->second;
}

}   // namespace NKikimr::NOlap::NReader::NCommon
