#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace perlin::server {

    // one remote peer; Send is only called from the tick thread
    class Connection {
    public:
        virtual ~Connection() = default;
        virtual bool Send(const uint8_t* data, size_t size, bool reliable) = 0;
        virtual void Close() = 0;
        virtual bool Alive() const = 0;
        virtual std::string Describe() const = 0;
        uint64_t id = 0;
        // the sdp offer this connection came from (nethernet), empty for local connections
        std::string offerSdp;
    };

    struct NetEvent {
        enum class Type : uint8_t { Open, Message, Close };
        Type type = Type::Message;
        std::shared_ptr<Connection> connection;
        std::vector<uint8_t> data;
    };

    // listeners run on their own threads and hand everything to the tick thread through here
    class EventQueue {
    public:
        void Push(NetEvent event) {
            std::lock_guard lock(mutex);
            events.push_back(std::move(event));
        }
        void Drain(std::vector<NetEvent>& out) {
            std::lock_guard lock(mutex);
            while (!events.empty()) {
                out.push_back(std::move(events.front()));
                events.pop_front();
            }
        }

    private:
        std::mutex mutex;
        std::deque<NetEvent> events;
    };

    class Listener {
    public:
        virtual ~Listener() = default;
        virtual const char* Name() const = 0;
        virtual bool Start(EventQueue& queue, std::string& error) = 0;
        virtual void Stop() = 0;
    };

    // webrtc data channels carry each message as [remaining fragments][payload]; this reassembles and splits them
    constexpr size_t kFragmentSize = 262143;

    class FragmentAssembler {
    public:
        // true when a whole message is ready in `out`
        bool Push(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
            if (size == 0)
                return false;
            buffer.insert(buffer.end(), data + 1, data + size);
            if (data[0] != 0)
                return false;
            out = std::move(buffer);
            buffer.clear();
            return true;
        }

    private:
        std::vector<uint8_t> buffer;
    };

    // fragments of at most kFragmentSize, each prefixed with how many follow; empty when it needs more than 256
    std::vector<std::vector<uint8_t>> SplitFragments(const uint8_t* data, size_t size);
}
