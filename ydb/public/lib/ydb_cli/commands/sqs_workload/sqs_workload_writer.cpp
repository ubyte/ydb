#include "sqs_workload_writer.h"
#include "consts.h"
#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/client/AsyncCallerContext.h>
#include <aws/core/utils/UUID.h>
#include <aws/sqs/SQSClient.h>
#include <aws/sqs/model/SendMessageBatchRequest.h>
#include <aws/sqs/model/SendMessageRequest.h>
#include <ydb/public/lib/ydb_cli/common/command.h>

#include <fmt/format.h>

namespace NYdb::NConsoleClient {

    namespace {

        void DecrementStartedCountAndNotify(const TSqsWorkloadWriterParams& params) {
            //std::unique_lock locker(*params.Mutex);
            --(*params.StartedCount);
            //params.FinishedCond->notify_all();
            params.FinishedCond->notify_one();
        }

        Aws::Vector<Aws::SQS::Model::SendMessageBatchRequestEntry>
        CreateSendMessageBatchRequestEntries(const TSqsWorkloadWriterParams& params, std::function<std::optional<std::string>()>& genMessageGroupID, ui32 messageDeduplicationID) {
            Aws::Vector<Aws::SQS::Model::SendMessageBatchRequestEntry> entries;
            for (ui32 i = 0; i < params.BatchSize; ++i) {
                Aws::String messageBody(params.MessageSize, 'a');
                Aws::SQS::Model::SendMessageBatchRequestEntry entry;
                entry.WithMessageBody(messageBody).WithId(fmt::format("{}", i));
                if (auto messageGroupId = genMessageGroupID()) {
                    entry.WithMessageGroupId(fmt::format("{}", *messageGroupId));
                }
                if (params.MaxUniqueMessages > 0) {
                    entry.WithMessageDeduplicationId(std::format("{}", messageDeduplicationID));
                }

                entries.push_back(std::move(entry));
            }
            return entries;
        }


        template <class T>
        T IncWrap(T& value, const T& max) {
            T res = value;
            if (++value >= max) {
                value = 0;
            }
            return res;
        }

        class TMessageGroupIdGenerator {
        public:
            explicit TMessageGroupIdGenerator(const TSqsWorkloadWriterParams& params, std::mt19937_64& rng)
                : GroupsPrefix(params.GroupsPrefix)
                , GroupsAmount(params.GroupsAmount)
                , GroupClientAmount(params.GroupClientAmount)
                , GroupClientSubdivide(params.GroupClientSubdivide)
                , TasksPerAdd(params.TasksPerAdd)
                , MessageGroupsDistribution(0, params.GroupsAmount - 1)
                , ClientIdDistribution(0, params.GroupClientAmount - 1)
                , TasksPerAddDistribution(1, params.TasksPerAdd)
                , ClientId(ClientIdDistribution(rng))
                , TasksInAdd(TasksPerAddDistribution(rng))
            {
                Y_ENSURE(params.TasksPerAdd >= 1);
                //Cerr << LabeledOutput(params.TasksPerAdd, params.GroupClientAmount,  params.GroupClientSubdivide) << "\n";

            }

            std::optional<std::string> Next(std::mt19937_64& rng) {
                auto c = ClientId;
                auto cs = ClientSubdiv;
                if (++TaskInAdd >= TasksInAdd) {
                    TaskInAdd = 0;
                    TasksInAdd = TasksPerAddDistribution(rng);
                    Y_ENSURE(TasksInAdd >= 1, LabeledOutput(TasksInAdd, TasksPerAdd));
                    Y_ENSURE(TasksInAdd <= TasksPerAdd, LabeledOutput(TasksInAdd, TasksPerAdd));
                    IncWrap(ClientId, GroupClientAmount);
                    if (ClientId == 0) {
                        IncWrap(ClientSubdiv, GroupClientSubdivide);
                    }
                }
                //Cerr << LabeledOutput(TasksInAdd, TaskInAdd, ClientId, ClientSubdiv) << "\n";
                return fmt::format("{}_c{}_sub{}_g{}", GroupsPrefix, c, cs, GetMessageGroupTail(rng));
            }

        private:
            std::string GetMessageGroupTail(std::mt19937_64& rng) {
                if (GroupsAmount > 0) {
                    return fmt::format("{}", MessageGroupsDistribution(rng));
                }
                if (GroupsAmount < 0) {
                    return fmt::format("{}", IncWrap(TailCounter, -GroupsAmount));
                }
                return "";
            }

            const TString GroupsPrefix;
            const i32 GroupsAmount;
            const i32 GroupClientAmount;
            const i32 GroupClientSubdivide;
            const i32 TasksPerAdd;

            std::uniform_int_distribution<ui32> MessageGroupsDistribution;
            std::uniform_int_distribution<i32> ClientIdDistribution;
            std::uniform_int_distribution<i32> TasksPerAddDistribution;

            i32 ClientId;
            i32 ClientSubdiv = 0;
            i32 TaskInAdd = 0;
            i32 TasksInAdd;
            i32 TailCounter = 0;
        };

    } // namespace

    void TSqsWorkloadWriter::OnMessageSent(
        const TSqsWorkloadWriterParams& params, const Aws::SQS::SQSClient*,
        const Aws::SQS::Model::SendMessageBatchRequest&,
        const Aws::SQS::Model::SendMessageBatchOutcome& outcome,
        const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) {
        auto failedCount = outcome.GetResult().GetFailed().size();
        if (!outcome.IsSuccess() || failedCount > 0) {
            params.Log->Write(
                ELogPriority::TLOG_ERR,
                TStringBuilder()
                    << "Error sending message: " << outcome.GetError().GetMessage()
                    << " failed: " << failedCount);
            params.StatsCollector->AddSendRequestErrorEvent(
                TSqsWorkloadStats::SendRequestErrorEvent());
        }

        auto successCount = outcome.GetResult().GetSuccessful().size();
        params.StatsCollector->AddSentMessagesEvent(
            TSqsWorkloadStats::SentMessagesEvent{successCount * params.MessageSize,
                                                 successCount});
        DecrementStartedCountAndNotify(params);
    }

    void TSqsWorkloadWriter::RunLoop(const TSqsWorkloadWriterParams& params,
                                     TInstant endTime) {
        std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<ui32> messageDeduplicationDistribution(0, params.MaxUniqueMessages - 1);

        std::function<std::optional<std::string>()> genMessageGroupID = [](){ return std::nullopt; };
        if (params.GroupsAmount != 0) {
            genMessageGroupID = [&rng, generator = TMessageGroupIdGenerator(params, rng)]() mutable { return generator.Next(rng); };
        }

        const TInstant startTime = Now();
        ui64 messagesDispatched = 0;

        while (Now() < endTime && !params.ErrorFlag->load()) {
            if (params.MessageCount.Defined()) {
                if (messagesDispatched >= *params.MessageCount) {
                    break;
                }
            }
            if (params.MessagesPerSec.Defined()) {
                const TInstant expectedTime = startTime + TDuration::Seconds(messagesDispatched / *params.MessagesPerSec);
                SleepUntil(expectedTime);
            }
            messagesDispatched += params.BatchSize;

            Aws::SQS::Model::SendMessageBatchRequest sendMessageBatchRequest;
            sendMessageBatchRequest.SetQueueUrl(params.QueueUrl.c_str());
            sendMessageBatchRequest.SetEntries(CreateSendMessageBatchRequestEntries(params, genMessageGroupID, messageDeduplicationDistribution(rng)));
            sendMessageBatchRequest.SetAdditionalCustomHeaderValue(
                AMZ_TARGET_HEADER, SQS_TARGET_SEND_MESSAGE_BATCH);

            {
                std::unique_lock locker(*params.Mutex);
                // WorkersCount tasks are running in parallel and also WorkersCount tasks are waiting in executor queue
                params.FinishedCond->wait(locker, [&params]() { return *params.StartedCount < params.WorkersCount * 2; });

                ++(*params.StartedCount);
            }

            params.StatsCollector->AddPushAsyncRequestTaskToQueueEvent(
                TSqsWorkloadStats::PushAsyncRequestTaskToQueueEvent());
            params.SqsClient->SendMessageBatchAsync(
                sendMessageBatchRequest,
                [&params](
                    const Aws::SQS::SQSClient* sqsClient,
                    const Aws::SQS::Model::SendMessageBatchRequest& sendMessageBatchRequest,
                    const Aws::SQS::Model::SendMessageBatchOutcome& outcome,
                    const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context) {
                    OnMessageSent(params, sqsClient, sendMessageBatchRequest,
                                  outcome, context);
                });
        }
    }

} // namespace NYdb::NConsoleClient
