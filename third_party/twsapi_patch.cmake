# ---- IBKR TWS API: fix four real bugs in IB's own socket client -------------
#
# The TWS API sources are downloaded at configure time and are NOT redistributed
# in this repository (IB API Non-Commercial License), so we ship the diff rather
# than the patched file. This script rewrites EReader.cpp and EClientSocket.cpp
# in the FetchContent source tree, in place, every configure. It is idempotent
# (sentinel guard) and it FATAL_ERRORs if any anchor stops matching, so bumping
# the twsapi URL/hash can never silently drop the fix.
#
# All four are covered by engine/tests/test_ereader_deadlock.cpp, which hangs or
# fails on an unpatched build.
#
# WHY (diagnosed 2026-08-02 after a wedged tws-data session; the same teardown
# is used by the ORDER and TICK clients, so this is not cosmetic):
#
#   EClientSocket::eDisconnect() joins the reader thread BEFORE closing the
#   socket:
#       if (m_pEReader) m_pEReader->stop();   // WaitForSingleObject(INFINITE)
#       if (m_fd >= 0)  SocketClose(m_fd);    // ...only after the join
#   EReader::stop() sets m_isAlive=false and waits forever, but the reader's
#   inner loop in bufferedRead() never re-tests m_isAlive; its only exit is
#   "select timed out AND !isSocketOK()", and isSocketOK() is just m_fd >= 0 --
#   still true, because the close is queued behind the join. If the reader is
#   parked mid-message when the peer goes silent (market closed / gateway
#   restart), that is a permanent mutual deadlock: eDisconnect waits for the
#   reader, the reader waits for bytes, the bytes need the socket closed, the
#   close needs eDisconnect to return.
#
#   Independently, eDisconnect() is reachable FROM the reader thread itself --
#   EClientSocket::onClose() on a peer FIN, and EReader::processNonBlockingSelect()
#   on a select error. IB guards against the resulting self-join on IB_POSIX
#   (pthread_equal) but forgot the IB_WIN32 branch, so on Windows the reader
#   ends up in WaitForSingleObject() on its own handle -- instant permanent
#   deadlock, with no log output at all and m_connState left at CS_CONNECTED so
#   callers never even see a disconnect.
#
#   Finally, EReader's ctor calls clientSocket->registerEReader(this) but the
#   dtor never un-registers. EClientSocket::eConnectImpl() builds an EReader on
#   the STACK, so from the moment eConnect() returns, EClientSocket::m_pEReader
#   dangles until a heap reader is constructed -- and any eDisconnect() in that
#   window (e.g. our connect watchdog aborting an in-flight connect) calls
#   stop() on freed stack memory.
#
# Fixes 1 and 2 must ship together: fix 1 routes every teardown through
# readToQueue()'s tail, which calls handleSocketError() -> possibly
# eDisconnect() on the reader thread, i.e. straight into the self-join that
# fix 2 removes.

set(_tws_client "${twsapi_src_SOURCE_DIR}/IBJts/source/cppclient/client")

# Replace exactly one occurrence of an anchor in _src, and prove it was unique.
function(_tt_patch_hunk name anchor replacement)
    string(FIND "${_src}" "${anchor}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR
            "twsapi patch: anchor '${name}' no longer matches ${_file}.\n"
            "The vendored TWS API changed. Re-derive the hunk in "
            "third_party/twsapi_patch.cmake before building -- do NOT skip it: "
            "it fixes three wedges that freeze the live order path.")
    endif()
    string(REPLACE "${anchor}" "${replacement}" _out "${_src}")
    string(FIND "${_out}" "${anchor}" _again)
    if(NOT _again EQUAL -1)
        message(FATAL_ERROR "twsapi patch: anchor '${name}' is not unique in ${_file}")
    endif()
    set(_src "${_out}" PARENT_SCOPE)
endfunction()

# Read a source file into _src, or set _skip if it is already patched.
macro(_tt_patch_open filename)
    set(_file "${_tws_client}/${filename}")
    set(_skip FALSE)
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "twsapi patch: ${_file} not found (twsapi layout changed?)")
    endif()
    file(READ "${_file}" _src)
    # Normalise first: CMake's file(WRITE) may emit CRLF on Windows, and the
    # anchors below are LF-joined. Without this, a second configure over an
    # already-written file would fail to match.
    string(REPLACE "\r\n" "\n" _src "${_src}")
    string(FIND "${_src}" "TT-PATCH" _sentinel)
    if(NOT _sentinel EQUAL -1)
        message(STATUS "twsapi patch: ${filename} already patched")
        set(_skip TRUE)
    endif()
endmacro()

_tt_patch_open("EReader.cpp")
if(NOT _skip)

# --- 1. bufferedRead(): let stop() actually stop the reader -------------------
_tt_patch_hunk("bufferedRead"
"    while (m_buf.size() < size && m_buf.size() < m_nMaxBufSize) {
      if (!processNonBlockingSelect() && !m_pClientSocket->isSocketOK())
        return false;
    }"
"    while (m_buf.size() < size && m_buf.size() < m_nMaxBufSize) {
      if (!m_isAlive) return false; // TT-PATCH: bound stop() at one select (~100ms)
      if (!processNonBlockingSelect() && !m_pClientSocket->isSocketOK())
        return false;
    }")

# --- 1b. readSingleMsg(): identical unbounded spin on the non-V100 path -------
# Dead today (m_useV100Plus defaults true) but one line, and it keeps the two
# loops from drifting apart.
_tt_patch_hunk("readSingleMsg"
"    while (msgSize == 0)
    {
      if (m_buf.size() >= m_nMaxBufSize * 3 / 4)"
"    while (msgSize == 0)
    {
      if (!m_isAlive) return 0; // TT-PATCH: same bound as bufferedRead()
      if (m_buf.size() >= m_nMaxBufSize * 3 / 4)")

# --- 2. stop(): never join our own thread ------------------------------------
# m_isAlive is stored OUTSIDE the guard so a self-call still winds the reader
# down on its next loop iteration instead of doing nothing at all.
_tt_patch_hunk("stop"
"#elif defined(IB_WIN32)
  if (m_hReadThread) {
    m_isAlive = false;
    WaitForSingleObject(m_hReadThread, INFINITE);
  }
#endif"
"#elif defined(IB_WIN32)
  // TT-PATCH: the IB_POSIX branch above guards the self-join with
  // pthread_equal; this one did not. eDisconnect() reaches stop() from the
  // reader thread itself (EClientSocket::onClose on a peer FIN, and
  // processNonBlockingSelect on a select error), so an unguarded
  // WaitForSingleObject(INFINITE) waits on its own handle -- forever.
  m_isAlive = false;
  if (m_hReadThread && GetThreadId(m_hReadThread) != GetCurrentThreadId()) {
    WaitForSingleObject(m_hReadThread, INFINITE);
  }
#endif")

# --- 3. ~EReader(): same self-guard, and un-register from the socket ----------
_tt_patch_hunk("dtor"
"#elif defined(IB_WIN32)
  if (m_hReadThread) {
    m_pClientSocket->eDisconnect();
  }
#endif
}"
"#elif defined(IB_WIN32)
  // TT-PATCH: self-thread guard, as in stop().
  if (m_hReadThread && GetThreadId(m_hReadThread) != GetCurrentThreadId()) {
    m_pClientSocket->eDisconnect();
  }
#endif
  // TT-PATCH: the ctor registers this reader with the socket and IB never
  // un-registers it. eConnectImpl() builds its EReader on the stack, so
  // EClientSocket::m_pEReader dangles from the moment eConnect() returns until
  // a heap reader is constructed -- and eDisconnect() in that window (our
  // connect watchdog) calls stop() on freed memory. m_pEReader is read only by
  // eDisconnect(), so clearing it here is safe.
  m_pClientSocket->registerEReader(nullptr);
}")

file(WRITE "${_file}" "${_src}")
message(STATUS "twsapi patch: EReader.cpp patched (4 hunks)")
endif()

# --- 4. EClientSocket::receive(): stop swallowing every socket error ----------
# handleSocketError() only copies WSAGetLastError() into errno under
# #ifdef _MSC_VER. This is a MinGW/UCRT64 build, so it reads the CRT errno --
# which Winsock never touches -- and takes its `if (errno == 0) return true;`
# path on EVERY error. A peer RST therefore came back as "no error": receive()
# returned 0 without calling onClose(), onReceive() left m_buf untouched, and
# select() kept reporting the reset fd readable, so processNonBlockingSelect()
# returned true immediately and bufferedRead() spun with no 100 ms timeout at
# all -- a pegged core on a dead socket, isSocketOK() permanently true, and no
# reconnect ever. Reproduced in engine/tests/test_ereader_deadlock.cpp.
#
# Deliberately NOT fixed by widening that #ifdef to _WIN32: mingw-w64's
# EWOULDBLOCK/ECONNREFUSED/EISCONN are not the WSAE* values, so every routine
# WSAEWOULDBLOCK from onSend() would fall through to eDisconnect(). Test the
# Winsock code directly instead, and only where recv() reports it.
_tt_patch_open("EClientSocket.cpp")
if(NOT _skip)

_tt_patch_hunk("receive"
"  int nResult = ::recv(m_fd, buf, sz, 0);

  if (nResult == -1 && !handleSocketError()) {
    return -1;
  }"
"  int nResult = ::recv(m_fd, buf, sz, 0);

#ifdef _WIN32
  // TT-PATCH: see third_party/twsapi_patch.cmake. handleSocketError() cannot
  // see Winsock errors on MinGW, so classify them here. Everything except a
  // would-block/interrupted read is fatal for this connection.
  if (nResult == -1) {
    const int _ttWsaErr = WSAGetLastError();
    if (_ttWsaErr != WSAEWOULDBLOCK && _ttWsaErr != WSAEINTR && _ttWsaErr != WSAEINPROGRESS) {
      onClose();   // safe on the reader thread now that stop() guards self-join
      return 0;
    }
  }
#endif

  if (nResult == -1 && !handleSocketError()) {
    return -1;
  }")

file(WRITE "${_file}" "${_src}")
message(STATUS "twsapi patch: EClientSocket.cpp patched (1 hunk)")
endif()
