#pragma once

#include <Common/ConcurrentBoundedQueue.h>
#include <common/logger_useful.h>
#include <common/types.h>
#include <grpcpp/server_context.h>
#include <kvproto/mpp.pb.h>
#include <kvproto/tikvpb.grpc.pb.h>

#include <boost/noncopyable.hpp>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace DB
{
class MPPTask;
using MPPDataPacketPtr = std::shared_ptr<mpp::MPPDataPacket>;

class MPPTunnel : private boost::noncopyable
{
public:
    MPPTunnel(
        const mpp::TaskMeta & receiver_meta_,
        const mpp::TaskMeta & sender_meta_,
        const std::chrono::seconds timeout_,
        const std::shared_ptr<MPPTask> & current_task_,
        int input_steams_num_);

    ~MPPTunnel();

    const String & id() const { return tunnel_id; }

    bool isTaskCancelled();

    // write a single packet to the tunnel, it will block if tunnel is not ready.
    void write(const mpp::MPPDataPacket & data, bool close_after_write = false);

    /// to avoid being blocked when pop(), we should send nullptr into send_queue
    void sendLoop();

    // finish the writing.
    void writeDone();

    /// close() finishes the tunnel, if the tunnel is connected already, it will
    /// write the error message to the tunnel, otherwise it just close the tunnel
    void close(const String & reason);

    // a MPPConn request has arrived. it will build connection by this tunnel;
    void connect(::grpc::ServerWriter<::mpp::MPPDataPacket> * writer_);

    // wait until all the data has been transferred.
    void waitForFinish();

private:
    void waitUntilConnectedOrCancelled(std::unique_lock<std::mutex> & lk);

    // must under mu's protection
    void finishWithLock();

    std::mutex mu;
    std::condition_variable cv_for_connected;
    std::condition_variable cv_for_finished;

    bool connected; // if the exchange in has connected this tunnel.

    std::atomic<bool> finished; // if the tunnel has finished its connection.

    ::grpc::ServerWriter<::mpp::MPPDataPacket> * writer;

    std::chrono::seconds timeout;

    std::weak_ptr<MPPTask> current_task;

    // tunnel id is in the format like "tunnel[sender]+[receiver]"
    String tunnel_id;

    int input_streams_num;

    std::unique_ptr<std::thread> send_thread;

    ConcurrentBoundedQueue<MPPDataPacketPtr> send_queue;

    Poco::Logger * log;
};

using MPPTunnelPtr = std::shared_ptr<MPPTunnel>;

} // namespace DB