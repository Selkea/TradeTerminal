// Regression tests for third_party/twsapi_patch.cmake.
//
// IB's own socket client has three ways to freeze a TWS session permanently.
// All three are reproduced here against a loopback stub, and all three wedge on
// an unpatched build - which is why each case bounds its own wait and hard-exits
// instead of blocking ctest. (Verified: built against the pristine twsapi
// sources, the first three cases fail; against the patched build all pass.)
// The last case covers a dangling-pointer bug in the same file; it is a latent
// UAF on a dead stack frame, so it does NOT fail without the patch - it is here
// to pin the intended lifetime, not to prove the fix.
//
// The stub never speaks the TWS wire protocol: the client is put into
// asyncEConnect mode, which skips the handshake entirely (EClientSocket.cpp
// `if (!m_asyncEConnect)`), so an accepted TCP connection is all we need to get
// a live EClientSocket + EReader in exactly the states that wedge.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <winsock2.h>  // must precede anything that pulls in <Windows.h>
#include <ws2tcpip.h>

#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

namespace {

// Records the one callback these tests care about.
struct Wrapper : DefaultEWrapper {
    std::atomic<bool> closed{false};
    void connectionClosed() override { closed.store(true); }
};

class StubPeer {
public:
    enum Mode {
        PartialThenSilent,  // 2 bytes of a 4-byte length prefix, then nothing
        GracefulFin,        // drain, then shutdown(SD_SEND) - a real FIN
        AbortiveReset,      // close with unread data pending - Winsock sends RST
    };

    explicit StubPeer(Mode mode) : mode_(mode) {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);

        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listener_ != INVALID_SOCKET);

        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = 0;  // let the OS pick
        REQUIRE(bind(listener_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
        REQUIRE(listen(listener_, 1) == 0);

        int len = sizeof(sa);
        REQUIRE(getsockname(listener_, reinterpret_cast<sockaddr*>(&sa), &len) == 0);
        port_ = ntohs(sa.sin_port);

        th_ = std::thread([this] { run(); });
    }

    ~StubPeer() {
        if (th_.joinable()) th_.join();
        if (conn_ != INVALID_SOCKET) closesocket(conn_);
        if (listener_ != INVALID_SOCKET) closesocket(listener_);
    }

    unsigned short port() const { return port_; }

private:
    void run() {
        conn_ = accept(listener_, nullptr, nullptr);
        if (conn_ == INVALID_SOCKET) return;
        if (mode_ == PartialThenSilent) {
            // Half a length prefix. readSingleMsg() asks bufferedRead() for 4
            // bytes, gets 2, and parks in its inner loop; the connection stays
            // open (and silent) until this object is destroyed.
            const char half[2] = {0, 0};
            send(conn_, half, 2, 0);
        } else if (mode_ == GracefulFin) {
            // The client's API sign-on has to be consumed first: closing (or
            // even shutting down) a socket with unread data queued makes
            // Winsock send an RST instead of a FIN, which is a different bug.
            drain();
            shutdown(conn_, SD_SEND);
        } else {
            closesocket(conn_);  // sign-on still unread -> RST
            conn_ = INVALID_SOCKET;
        }
    }

    void drain() {
        DWORD tmo = 200;
        setsockopt(conn_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo),
                   sizeof(tmo));
        char sink[1024];
        while (recv(conn_, sink, sizeof(sink), 0) > 0) {}
    }

    Mode mode_;
    SOCKET listener_ = INVALID_SOCKET;
    SOCKET conn_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::thread th_;
};

// Poll until `pred` or the deadline. Returns whether it came true.
template <class Pred>
bool wait_until(Pred pred, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

}  // namespace

// Deadlock 1: eDisconnect() joins the reader BEFORE closing the socket, and
// bufferedRead()'s inner loop never re-tests m_isAlive. Its only exit is
// "select timed out AND !isSocketOK()", but isSocketOK() is just m_fd >= 0 -
// still true, because the close is queued behind the join. eDisconnect waits
// for the reader; the reader waits for bytes that need the socket closed.
TEST_CASE("eDisconnect returns while the reader is parked mid-message") {
    StubPeer peer(StubPeer::PartialThenSilent);
    Wrapper wrapper;
    EReaderOSSignal signal(1000);
    EClientSocket client(&wrapper, &signal);
    client.asyncEConnect(true);
    REQUIRE(client.eConnect("127.0.0.1", peer.port(), 0));

    auto reader = std::make_unique<EReader>(&client, &signal);
    reader->start();
    std::this_thread::sleep_for(400ms);  // let it park inside bufferedRead()

    // The call under test, on its own thread so a wedge fails the test instead
    // of hanging the suite.
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([&client, done] {
        client.eDisconnect();
        done->store(true);
    }).detach();

    const bool returned = wait_until([&] { return done->load(); }, 5000ms);
    CHECK(returned);
    if (!returned) {
        MESSAGE("eDisconnect() deadlocked - twsapi_patch.cmake did not apply");
        std::_Exit(1);  // the detached thread is wedged; unwinding would hang
    }
    CHECK_FALSE(client.isSocketOK());
}

// Deadlock 2: a peer FIN makes recv() return 0, which drives
// EClientSocket::onClose() -> eDisconnect() -> EReader::stop() ON THE READER
// THREAD. IB guards that self-join on IB_POSIX (pthread_equal) but not on
// IB_WIN32, so the reader ends up in WaitForSingleObject() on its own handle.
// Nothing else is involved: no partial message, no app-side call. And because
// the hang is upstream of eDisconnectBase(), the session stays CS_CONNECTED
// with no log output at all - invisible to every watchdog we have.
TEST_CASE("a peer FIN does not self-deadlock the reader thread") {
    StubPeer peer(StubPeer::GracefulFin);
    Wrapper wrapper;
    EReaderOSSignal signal(1000);
    EClientSocket client(&wrapper, &signal);
    client.asyncEConnect(true);
    REQUIRE(client.eConnect("127.0.0.1", peer.port(), 0));

    auto reader = std::make_unique<EReader>(&client, &signal);
    reader->start();

    // No main-thread involvement: the reader must tear itself down.
    const bool closed = wait_until([&] { return wrapper.closed.load(); }, 5000ms);
    CHECK(closed);
    if (!closed) {
        MESSAGE("reader thread self-deadlocked in stop() - twsapi_patch.cmake did not apply");
        std::_Exit(1);  // ~EReader would join the wedged thread
    }
    CHECK_FALSE(client.isConnected());
    CHECK_FALSE(client.isSocketOK());
}

// Wedge 3, found by the test above before it was fixed: handleSocketError()
// only copies WSAGetLastError() into errno under #ifdef _MSC_VER, so on MinGW it
// reads the CRT errno -- which Winsock never sets -- and reports "no error" for
// everything. An RST then made receive() return 0 without calling onClose(), so
// select() kept reporting the reset fd readable, processNonBlockingSelect()
// returned true with no timeout, and bufferedRead() spun at 100% CPU on a dead
// socket while isSocketOK() stayed true: no reconnect, ever.
TEST_CASE("a peer reset closes the session instead of spinning") {
    StubPeer peer(StubPeer::AbortiveReset);
    Wrapper wrapper;
    EReaderOSSignal signal(1000);
    EClientSocket client(&wrapper, &signal);
    client.asyncEConnect(true);
    REQUIRE(client.eConnect("127.0.0.1", peer.port(), 0));

    auto reader = std::make_unique<EReader>(&client, &signal);
    reader->start();

    const bool closed = wait_until([&] { return wrapper.closed.load(); }, 5000ms);
    CHECK(closed);
    if (!closed) {
        MESSAGE("reader spun on a reset socket - twsapi_patch.cmake did not apply");
        std::_Exit(1);
    }
    CHECK_FALSE(client.isConnected());
    CHECK_FALSE(client.isSocketOK());
}

// The ctor registers the reader with the socket; IB's dtor never un-registers,
// so EClientSocket::m_pEReader dangles at every eConnectImpl() (its EReader is
// a stack local) and any eDisconnect() in that window - our connect watchdog -
// calls stop() on freed memory. Destroying a reader and then disconnecting must
// be safe. Note this passes unpatched too: the freed frame is still readable and
// m_hReadThread happens to be 0 there, so nothing observable goes wrong without
// a sanitizer. It documents the contract; the patch is what enforces it.
TEST_CASE("eDisconnect after the reader is destroyed does not touch freed memory") {
    StubPeer peer(StubPeer::PartialThenSilent);
    Wrapper wrapper;
    EReaderOSSignal signal(1000);
    EClientSocket client(&wrapper, &signal);
    client.asyncEConnect(true);
    REQUIRE(client.eConnect("127.0.0.1", peer.port(), 0));

    {
        EReader reader(&client, &signal);  // never started, as in eConnectImpl
    }
    // Unpatched, m_pEReader still points at that dead stack frame here.
    client.eDisconnect();
    CHECK_FALSE(client.isSocketOK());
}
