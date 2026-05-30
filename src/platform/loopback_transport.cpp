#include "mvci/platform/transport.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>

namespace mvci {
namespace {

class LoopbackTransport final : public ITransport {
public:
  Status open(const std::string&) override {
    open_ = true;
    return STATUS_NOERROR;
  }

  void close() override {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    std::queue<std::vector<std::uint8_t>> empty;
    rx_.swap(empty);
    tx_.swap(empty);
  }

  Status write(const std::vector<std::uint8_t>& packet) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) {
      return ERR_NOT_INITIALIZED;
    }
    tx_.push(packet);
    rx_.push(packet);
    condition_.notify_all();
    return STATUS_NOERROR;
  }

  Status read(std::vector<std::uint8_t>& packet, std::uint32_t timeoutMs) override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!open_) {
      return ERR_NOT_INITIALIZED;
    }

    const auto ready = condition_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !rx_.empty() || !open_; });
    if (!ready || !open_) {
      return ERR_TIMEOUT;
    }

    packet = std::move(rx_.front());
    rx_.pop();
    return STATUS_NOERROR;
  }

  Status controlTransfer(std::uint8_t,
                         std::uint8_t,
                         std::uint16_t,
                         std::uint16_t,
                         std::vector<std::uint8_t>&,
                         std::uint32_t) override {
    return ERR_NOT_SUPPORTED;
  }

  void clearRx() override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::queue<std::vector<std::uint8_t>> empty;
    rx_.swap(empty);
  }

  void clearTx() override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::queue<std::vector<std::uint8_t>> empty;
    tx_.swap(empty);
  }

private:
  bool open_{false};
  std::mutex mutex_;
  std::condition_variable condition_;
  std::queue<std::vector<std::uint8_t>> rx_;
  std::queue<std::vector<std::uint8_t>> tx_;
};

} // namespace

std::unique_ptr<ITransport> createLoopbackTransport() {
  return std::make_unique<LoopbackTransport>();
}

} // namespace mvci