#include "yql_generic_lookup_actor.h"
#include "yql_generic_token_provider.h"
#include "yql_generic_base_actor.h"

#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/actorsystem.h>
#include <ydb/library/actors/core/event_local.h>
#include <ydb/library/actors/core/events.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/core/log.h>
#include <ydb/library/mkql_proto/mkql_proto.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>
#include <ydb/library/yql/dq/runtime/dq_arrow_helpers.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders.h>
#include <yql/essentials/minikql/mkql_node_builder.h>
#include <ydb/library/yql/providers/generic/proto/source.pb.h>
#include <ydb/library/yql/providers/generic/connector/libcpp/error.h>
#include <ydb/library/yql/providers/generic/connector/libcpp/utils.h>
#include <ydb/library/yql/providers/generic/proto/range.pb.h>
#include <yql/essentials/providers/common/provider/yql_provider_names.h>
#include <yql/essentials/public/udf/arrow/util.h>
#include <yql/essentials/utils/log/log.h>
#include <yql/essentials/utils/yql_panic.h>
#include <ydb/core/formats/arrow/serializer/abstract.h>

namespace NYql::NDq {

    using namespace NActors;

    namespace {

        const NKikimr::NMiniKQL::TStructType* MergeStructTypes(const NKikimr::NMiniKQL::TTypeEnvironment& env, const NKikimr::NMiniKQL::TStructType* t1, const NKikimr::NMiniKQL::TStructType* t2) {
            Y_ABORT_UNLESS(t1);
            Y_ABORT_UNLESS(t2);
            NKikimr::NMiniKQL::TStructTypeBuilder resultTypeBuilder{env};
            for (ui32 i = 0; i != t1->GetMembersCount(); ++i) {
                resultTypeBuilder.Add(t1->GetMemberName(i), t1->GetMemberType(i));
            }
            for (ui32 i = 0; i != t2->GetMembersCount(); ++i) {
                resultTypeBuilder.Add(t2->GetMemberName(i), t2->GetMemberType(i));
            }
            return resultTypeBuilder.Build();
        }

        template <typename T>
        T ExtractFromConstFuture(const NThreading::TFuture<T>& f) {
            // We want to avoid making a copy of data stored in a future.
            // But there is no direct way to extract data from a const future5
            // So, we make a copy of the future, that is cheap. Then, extract the value from this copy.
            // It destructs the value in the original future, but this trick is legal and documented here:
            // https://docs.yandex-team.ru/arcadia-cpp/cookbook/concurrency
            return NThreading::TFuture<T>(f).ExtractValueSync();
        }

    } // namespace

    class TGenericLookupActor
        : public NYql::NDq::IDqAsyncLookupSource,
          public TGenericBaseActor<TGenericLookupActor> {
        using TBase = TGenericBaseActor<TGenericLookupActor>;

    public:
        TGenericLookupActor(
            NConnector::IClient::TPtr connectorClient,
            TGenericTokenProvider::TPtr tokenProvider,
            NActors::TActorId&& parentId,
            ::NMonitoring::TDynamicCounterPtr taskCounters,
            std::shared_ptr<NKikimr::NMiniKQL::TScopedAlloc> alloc,
            std::shared_ptr<IDqAsyncLookupSource::TKeyTypeHelper> keyTypeHelper,
            NYql::Generic::TLookupSource&& lookupSource,
            const NKikimr::NMiniKQL::TStructType* keyType,
            const NKikimr::NMiniKQL::TStructType* payloadType,
            const NKikimr::NMiniKQL::TTypeEnvironment& typeEnv,
            const NKikimr::NMiniKQL::THolderFactory& holderFactory,
            const size_t maxKeysInRequest)
            : Connector(connectorClient)
            , TokenProvider(std::move(tokenProvider))
            , ParentId(std::move(parentId))
            , Alloc(alloc)
            , KeyTypeHelper(keyTypeHelper)
            , LookupSource(std::move(lookupSource))
            , KeyType(keyType)
            , PayloadType(payloadType)
            , SelectResultType(MergeStructTypes(typeEnv, keyType, payloadType))
            , HolderFactory(holderFactory)
            , ColumnDestinations(CreateColumnDestination())
            , MaxKeysInRequest(maxKeysInRequest)
        {
            InitMonCounters(taskCounters);
        }

        ~TGenericLookupActor() {
            Free();
        }

    private:
        void Free() {
            auto guard = Guard(*Alloc);
            if (Request && InFlight) {
                // If request fails on (unrecoverable) error or cancelled, we may end up with non-zero InFlight (when request successfully completed, @Request is nullptr)
                InFlight->Dec();
            }
            Request.reset();
            KeyTypeHelper.reset();
        }
        void InitMonCounters(const ::NMonitoring::TDynamicCounterPtr& taskCounters) {
            if (!taskCounters) {
                return;
            }
            auto component = taskCounters->GetSubgroup("component", "LookupSrc");
            Count = component->GetCounter("Reqs");
            Keys = component->GetCounter("Keys");
            ResultChunks = component->GetCounter("Chunks");
            ResultRows = component->GetCounter("Rows");
            ResultBytes = component->GetCounter("Bytes");
            AnswerTime = component->GetCounter("AnswerMs");
            CpuTime = component->GetCounter("CpuUs");
            InFlight = component->GetCounter("InFlight");
        }
    public:

        void Bootstrap() {
            auto dsi = LookupSource.data_source_instance();
            YQL_CLOG(INFO, ProviderGeneric) << "New generic proivider lookup source actor(ActorId=" << SelfId() << ") for"
                                            << " kind=" << NYql::EGenericDataSourceKind_Name(dsi.kind())
                                            << ", endpoint=" << dsi.endpoint().ShortDebugString()
                                            << ", database=" << dsi.database()
                                            << ", use_tls=" << ToString(dsi.use_tls())
                                            << ", protocol=" << NYql::EGenericProtocol_Name(dsi.protocol())
                                            << ", table=" << LookupSource.table();
            Become(&TGenericLookupActor::StateFunc);
        }

        static constexpr char ActorName[] = "GENERIC_PROVIDER_LOOKUP_ACTOR";

    private: // IDqAsyncLookupSource
        size_t GetMaxSupportedKeysInRequest() const override {
            return MaxKeysInRequest;
        }
        void AsyncLookup(std::weak_ptr<IDqAsyncLookupSource::TUnboxedValueMap> request) override {
            auto guard = Guard(*Alloc);
            CreateRequest(request.lock());
        }
        void PassAway() override {
            Free();
            TBase::PassAway();
        }

    private: // events
        STRICT_STFUNC(StateFunc,
                      hFunc(TEvLookupRequest, Handle);
                      hFunc(TEvListSplitsIterator, Handle);
                      hFunc(TEvListSplitsPart, Handle);
                      hFunc(TEvReadSplitsIterator, Handle);
                      hFunc(TEvReadSplitsPart, Handle);
                      hFunc(TEvReadSplitsFinished, Handle);
                      hFunc(TEvError, Handle);
                      hFunc(NActors::TEvents::TEvPoison, Handle);)

        void Handle(TEvListSplitsIterator::TPtr ev) {
            auto& iterator = ev->Get()->Iterator;
            iterator->ReadNext().Subscribe(
                [actorSystem = TActivationContext::ActorSystem(), selfId = SelfId()](const NConnector::TAsyncResult<NConnector::NApi::TListSplitsResponse>& asyncResult) {
                    YQL_CLOG(DEBUG, ProviderGeneric) << "ActorId=" << selfId << " Got TListSplitsResponse from Connector";
                    auto result = ExtractFromConstFuture(asyncResult);
                    if (result.Status.Ok()) {
                        Y_ABORT_UNLESS(result.Response);
                        auto ev = new TEvListSplitsPart(std::move(*result.Response));
                        actorSystem->Send(new NActors::IEventHandle(selfId, selfId, ev));
                    } else {
                        SendError(actorSystem, selfId, result.Status);
                    }
                });
        }

        void Handle(TEvListSplitsPart::TPtr ev) {
            auto response = ev->Get()->Response;
            Y_ABORT_UNLESS(response.splits_size() == 1);
            auto& split = response.splits(0);
            NConnector::NApi::TReadSplitsRequest readRequest;

            *readRequest.mutable_data_source_instance() = LookupSource.data_source_instance();
            auto error = TokenProvider->MaybeFillToken(*readRequest.mutable_data_source_instance());
            if (error) {
                SendError(TActivationContext::ActorSystem(), SelfId(), std::move(error));
                return;
            }

            *readRequest.add_splits() = split;
            readRequest.Setformat(NConnector::NApi::TReadSplitsRequest_EFormat::TReadSplitsRequest_EFormat_ARROW_IPC_STREAMING);
            readRequest.set_filtering(NConnector::NApi::TReadSplitsRequest::FILTERING_MANDATORY);
            Connector->ReadSplits(readRequest).Subscribe([actorSystem = TActivationContext::ActorSystem(), selfId = SelfId()](const NConnector::TReadSplitsStreamIteratorAsyncResult& asyncResult) {
                YQL_CLOG(DEBUG, ProviderGeneric) << "ActorId=" << selfId << " Got ReadSplitsStreamIterator from Connector";
                auto result = ExtractFromConstFuture(asyncResult);
                if (result.Status.Ok()) {
                    auto ev = new TEvReadSplitsIterator(std::move(result.Iterator));
                    actorSystem->Send(new NActors::IEventHandle(selfId, selfId, ev));
                } else {
                    SendError(actorSystem, selfId, result.Status);
                }
            });
        }

        void Handle(TEvReadSplitsIterator::TPtr ev) {
            ReadSplitsIterator = ev->Get()->Iterator;
            ReadNextData();
        }

        void Handle(TEvReadSplitsPart::TPtr ev) {
            ProcessReceivedData(ev->Get()->Response);
            ReadNextData();
        }

        void Handle(TEvReadSplitsFinished::TPtr) {
            FinalizeRequest();
        }

        void Handle(TEvError::TPtr ev) {
            auto actorSystem = TActivationContext::ActorSystem();
            auto error = ev->Get()->Error;
            auto errEv = std::make_unique<IDqComputeActorAsyncInput::TEvAsyncInputError>(
                                  -1,
                                  NConnector::ErrorToIssues(error),
                                  NConnector::ErrorToDqStatus(error));
            actorSystem->Send(new NActors::IEventHandle(ParentId, SelfId(), errEv.release()));
        }

        void Handle(NActors::TEvents::TEvPoison::TPtr) {
            PassAway();
        }

        void Handle(TEvLookupRequest::TPtr ev) {
            auto guard = Guard(*Alloc);
            CreateRequest(ev->Get()->Request.lock());
        }

    private:
        static TDuration GetCpuTimeDelta(ui64 startCycleCount) {
            return TDuration::Seconds(NHPTimer::GetSeconds(GetCycleCountFast() - startCycleCount));
        }

        void CreateRequest(std::shared_ptr<IDqAsyncLookupSource::TUnboxedValueMap> request) {
            if (!request) {
                return;
            }
            auto startCycleCount = GetCycleCountFast();
            SentTime = TInstant::Now();
            YQL_CLOG(DEBUG, ProviderGeneric) << "ActorId=" << SelfId() << " Got LookupRequest for " << request->size() << " keys";
            Y_ABORT_IF(request->size() == 0 || request->size() > MaxKeysInRequest);

            if (Count) {
                Count->Inc();
                InFlight->Inc();
                Keys->Add(request->size());
            }

            Request = std::move(request);
            NConnector::NApi::TListSplitsRequest splitRequest;

            auto error = FillSelect(*splitRequest.add_selects());
            if (error) {
                SendError(TActivationContext::ActorSystem(), SelfId(), std::move(error));
                return;
            };

            splitRequest.Setmax_split_count(1);
            Connector->ListSplits(splitRequest).Subscribe([actorSystem = TActivationContext::ActorSystem(), selfId = SelfId()](const NConnector::TListSplitsStreamIteratorAsyncResult& asyncResult) {
                auto result = ExtractFromConstFuture(asyncResult);
                if (result.Status.Ok()) {
                    YQL_CLOG(DEBUG, ProviderGeneric) << "ActorId=" << selfId << " Got TListSplitsStreamIterator";
                    Y_ABORT_UNLESS(result.Iterator, "Uninitialized iterator");
                    auto ev = new TEvListSplitsIterator(std::move(result.Iterator));
                    actorSystem->Send(new NActors::IEventHandle(selfId, selfId, ev));
                } else {
                    SendError(actorSystem, selfId, result.Status);
                }
            });
            if (CpuTime) {
                CpuTime->Add(GetCpuTimeDelta(startCycleCount).MicroSeconds());
            }
        }

        void ReadNextData() {
            ReadSplitsIterator->ReadNext().Subscribe(
                [actorSystem = TActivationContext::ActorSystem(), selfId = SelfId()](const NConnector::TAsyncResult<NConnector::NApi::TReadSplitsResponse>& asyncResult) {
                    auto result = ExtractFromConstFuture(asyncResult);
                    if (result.Status.Ok()) {
                        YQL_CLOG(DEBUG, ProviderGeneric) << "ActorId=" << selfId << " Got DataChunk";
                        Y_ABORT_UNLESS(result.Response);
                        auto& response = *result.Response;
                        if (NConnector::IsSuccess(response)) {
                            auto ev = new TEvReadSplitsPart(std::move(response));
                            actorSystem->Send(new NActors::IEventHandle(selfId, selfId, ev));
                        } else {
                            SendError(actorSystem, selfId, response.Geterror());
                        }
                    } else if (NConnector::GrpcStatusEndOfStream(result.Status)) {
                        YQL_CLOG(DEBUG, ProviderGeneric) << "ActorId=" << selfId << " Got EOF";
                        auto ev = new TEvReadSplitsFinished(std::move(result.Status));
                        actorSystem->Send(new NActors::IEventHandle(selfId, selfId, ev));
                    } else {
                        SendError(actorSystem, selfId, result.Status);
                    }
                });
        }

        void ProcessReceivedData(const NConnector::NApi::TReadSplitsResponse& resp) {
            auto startCycleCount = GetCycleCountFast();
            Y_ABORT_UNLESS(resp.payload_case() == NConnector::NApi::TReadSplitsResponse::PayloadCase::kArrowIpcStreaming);
            if (ResultChunks) {
                ResultChunks->Inc();
                if (resp.has_stats()) {
                    ResultRows->Add(resp.stats().rows());
                    ResultBytes->Add(resp.stats().bytes());
                }
            }
            auto guard = Guard(*Alloc);
            NKikimr::NArrow::NSerialization::TSerializerContainer deser = NKikimr::NArrow::NSerialization::TSerializerContainer::GetDefaultSerializer(); // todo move to class' member
            const auto& data = deser->Deserialize(resp.arrow_ipc_streaming());
            Y_ABORT_UNLESS(data.ok());
            const auto& value = data.ValueOrDie();
            Y_ABORT_UNLESS(static_cast<ui32>(value->num_columns()) == ColumnDestinations.size());
            std::vector<NKikimr::NMiniKQL::TUnboxedValueVector> columns(ColumnDestinations.size());
            for (size_t i = 0; i != columns.size(); ++i) {
                Y_ABORT_UNLESS(value->column_name(i) == (ColumnDestinations[i].first == EColumnDestination::Key ? KeyType : PayloadType)->GetMemberName(ColumnDestinations[i].second));
                columns[i] = NArrow::ExtractUnboxedValues(value->column(i), SelectResultType->GetMemberType(i), HolderFactory);
            }

            auto height = columns[0].size();
            for (size_t i = 0; i != height; ++i) {
                NUdf::TUnboxedValue* keyItems;
                NUdf::TUnboxedValue key = HolderFactory.CreateDirectArrayHolder(KeyType->GetMembersCount(), keyItems);
                NUdf::TUnboxedValue* outputItems;
                NUdf::TUnboxedValue output = HolderFactory.CreateDirectArrayHolder(PayloadType->GetMembersCount(), outputItems);
                for (size_t j = 0; j != columns.size(); ++j) {
                    (ColumnDestinations[j].first == EColumnDestination::Key ? keyItems : outputItems)[ColumnDestinations[j].second] = columns[j][i];
                }
                if (auto* v = Request->FindPtr(key)) {
                    *v = std::move(output); // duplicates will be overwritten
                }
            }
            if (CpuTime) {
                CpuTime->Add(GetCpuTimeDelta(startCycleCount).MicroSeconds());
            }
        }

        void FinalizeRequest() {
            YQL_CLOG(DEBUG, ProviderGeneric) << "Sending lookup results for " << Request->size() << " keys";
            auto guard = Guard(*Alloc);
            auto ev = new IDqAsyncLookupSource::TEvLookupResult(Request);
            if (AnswerTime) {
                AnswerTime->Add((TInstant::Now() - SentTime).MilliSeconds());
                InFlight->Dec();
            }
            Request.reset();
            TActivationContext::ActorSystem()->Send(new NActors::IEventHandle(ParentId, SelfId(), ev));
            LookupResult = {};
            ReadSplitsIterator = {};
        }

        static void SendError(NActors::TActorSystem* actorSystem, const NActors::TActorId& selfId, const NConnector::NApi::TError& error) {
            YQL_CLOG(ERROR, ProviderGeneric) << "ActorId=" << selfId << " Got GrpcError from Connector:" << error.Getmessage();
            actorSystem->Send(
                selfId,
                new TEvError(std::move(error)));
        }

        static void SendError(NActors::TActorSystem* actorSystem, const NActors::TActorId& selfId, const NYdbGrpc::TGrpcStatus& status) {
            SendError(actorSystem, selfId, NConnector::ErrorFromGRPCStatus(status));
        }

        static void SendError(NActors::TActorSystem* actorSystem, const NActors::TActorId& selfId, TString error) {
            NConnector::NApi::TError dst;
            *dst.mutable_message() = error;
            SendError(actorSystem, selfId, std::move(dst));
        }

    private:
        enum class EColumnDestination {
            Key,
            Output
        };

        std::vector<std::pair<EColumnDestination, size_t>> CreateColumnDestination() {
            THashMap<TStringBuf, size_t> keyColumns;
            for (ui32 i = 0; i != KeyType->GetMembersCount(); ++i) {
                keyColumns[KeyType->GetMemberName(i)] = i;
            }
            THashMap<TStringBuf, size_t> outputColumns;
            for (ui32 i = 0; i != PayloadType->GetMembersCount(); ++i) {
                outputColumns[PayloadType->GetMemberName(i)] = i;
            }

            std::vector<std::pair<EColumnDestination, size_t>> result(SelectResultType->GetMembersCount());
            for (size_t i = 0; i != result.size(); ++i) {
                if (const auto* p = keyColumns.FindPtr(SelectResultType->GetMemberName(i))) {
                    result[i] = {EColumnDestination::Key, *p};
                } else if (const auto* p = outputColumns.FindPtr(SelectResultType->GetMemberName(i))) {
                    result[i] = {EColumnDestination::Output, *p};
                } else {
                    Y_ABORT();
                }
            }
            return result;
        }

        TString FillSelect(NConnector::NApi::TSelect& select) {
            auto dsi = LookupSource.data_source_instance();
            auto error = TokenProvider->MaybeFillToken(dsi);
            if (error) {
                return error;
            }
            *select.mutable_data_source_instance() = dsi;

            for (ui32 i = 0; i != SelectResultType->GetMembersCount(); ++i) {
                auto c = select.mutable_what()->add_items()->mutable_column();
                c->Setname((TString(SelectResultType->GetMemberName(i))));
                ExportTypeToProto(SelectResultType->GetMemberType(i), *c->mutable_type());
            }

            select.mutable_from()->Settable(LookupSource.table());

            NConnector::NApi::TPredicate_TDisjunction disjunction;
            for (const auto& [k, _] : *Request) {
                NConnector::NApi::TPredicate_TConjunction conjunction;
                for (ui32 c = 0; c != KeyType->GetMembersCount(); ++c) {
                    NConnector::NApi::TPredicate_TComparison eq;
                    eq.Setoperation(NConnector::NApi::TPredicate_TComparison_EOperation::TPredicate_TComparison_EOperation_EQ);
                    eq.mutable_left_value()->Setcolumn(TString(KeyType->GetMemberName(c)));
                    auto rightTypedValue = eq.mutable_right_value()->mutable_typed_value();
                    ExportTypeToProto(KeyType->GetMemberType(c), *rightTypedValue->mutable_type());
                    ExportValueToProto(KeyType->GetMemberType(c), k.GetElement(c), *rightTypedValue->mutable_value());
                    *conjunction.mutable_operands()->Add()->mutable_comparison() = eq;
                }
                *disjunction.mutable_operands()->Add()->mutable_conjunction() = conjunction;
            }
            *select.mutable_where()->mutable_filter_typed()->mutable_disjunction() = disjunction;
            return {};
        }

    private:
        NConnector::IClient::TPtr Connector;
        TGenericTokenProvider::TPtr TokenProvider;
        const NActors::TActorId ParentId;
        std::shared_ptr<NKikimr::NMiniKQL::TScopedAlloc> Alloc;
        std::shared_ptr<TKeyTypeHelper> KeyTypeHelper;
        const NYql::Generic::TLookupSource LookupSource;
        const NKikimr::NMiniKQL::TStructType* const KeyType;
        const NKikimr::NMiniKQL::TStructType* const PayloadType;
        const NKikimr::NMiniKQL::TStructType* const SelectResultType; // columns from KeyType + PayloadType
        const NKikimr::NMiniKQL::THolderFactory& HolderFactory;
        const std::vector<std::pair<EColumnDestination, size_t>> ColumnDestinations;
        const size_t MaxKeysInRequest;
        std::shared_ptr<IDqAsyncLookupSource::TUnboxedValueMap> Request;
        NConnector::IReadSplitsStreamIterator::TPtr ReadSplitsIterator; // TODO move me to TEvReadSplitsPart
        NKikimr::NMiniKQL::TKeyPayloadPairVector LookupResult;
        ::NMonitoring::TDynamicCounters::TCounterPtr Count;
        ::NMonitoring::TDynamicCounters::TCounterPtr Keys;
        ::NMonitoring::TDynamicCounters::TCounterPtr ResultRows;
        ::NMonitoring::TDynamicCounters::TCounterPtr ResultBytes;
        ::NMonitoring::TDynamicCounters::TCounterPtr ResultChunks;
        ::NMonitoring::TDynamicCounters::TCounterPtr AnswerTime;
        ::NMonitoring::TDynamicCounters::TCounterPtr CpuTime;
        ::NMonitoring::TDynamicCounters::TCounterPtr InFlight;
        TInstant SentTime;
    };

    std::pair<NYql::NDq::IDqAsyncLookupSource*, NActors::IActor*> CreateGenericLookupActor(
        NConnector::IClient::TPtr connectorClient,
        ISecuredServiceAccountCredentialsFactory::TPtr credentialsFactory,
        NActors::TActorId parentId,
        ::NMonitoring::TDynamicCounterPtr taskCounters,
        std::shared_ptr<NKikimr::NMiniKQL::TScopedAlloc> alloc,
        std::shared_ptr<IDqAsyncLookupSource::TKeyTypeHelper> keyTypeHelper,
        NYql::Generic::TLookupSource&& lookupSource,
        const NKikimr::NMiniKQL::TStructType* keyType,
        const NKikimr::NMiniKQL::TStructType* payloadType,
        const NKikimr::NMiniKQL::TTypeEnvironment& typeEnv,
        const NKikimr::NMiniKQL::THolderFactory& holderFactory,
        const size_t maxKeysInRequest)
    {
        auto tokenProvider = NYql::NDq::CreateGenericTokenProvider(lookupSource.GetToken(), lookupSource.GetServiceAccountId(), lookupSource.GetServiceAccountIdSignature(), credentialsFactory);
        auto guard = Guard(*alloc);
        const auto actor = new TGenericLookupActor(
            connectorClient,
            std::move(tokenProvider),
            std::move(parentId),
            taskCounters,
            alloc,
            keyTypeHelper,
            std::move(lookupSource),
            keyType,
            payloadType,
            typeEnv,
            holderFactory,
            maxKeysInRequest);
        return {actor, actor};
    }

} // namespace NYql::NDq
