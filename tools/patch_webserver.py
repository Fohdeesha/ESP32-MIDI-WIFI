"""Patch the Arduino core's WebServer library at build time.

arduino-esp32 2.0.17's WebServer handles one request at a time and reads it
completely, multipart body included, before any handler (or its password
check) runs. Nothing bounded how long that took:

- A file part was read byte by byte, waiting for each byte for as long as the
  client kept the connection open.
- Between multipart parts, and inside a plain field's value, a loop read lines
  with no exit for a client that stopped sending or hung up: it got "" back
  once a second, forever (and a field's value grew by a newline each time).
- Every other read (request line, headers, a form body) waited as long as the
  client kept trickling bytes.

Measured 2026-10-01: a 60-byte request from a client with no password (a
boundary, one junk line, then disconnect) hung the server, and the loop
watchdog reset the board 30 s later as TASK_WDT. The boot guard counts that as
a crash, so three of them inside 30 s of boot would also roll the firmware
back to the previous image.

Now every read while a request is parsed stops at a deadline 20 s after the
request began (under the 30 s loop watchdog), and a wait for data gives up
after HTTP_MAX_POST_WAIT (5 s) of silence. The one exception is the body of an
upload whose handler calls server.allowLongUpload() -- an authorized firmware
image, which can take longer on a slow link and feeds the watchdog per chunk.
A handler can also stop an upload it has already answered with
server.abortUpload(). An aborted upload's handler gets UPLOAD_FILE_ABORTED,
as for a dropped connection; that now also happens when a request carries more
fields than the parser holds after a file part, which used to end the request
without telling the handler.

The framework's own copy is never touched. Each build copies the library into
the build directory, patches it there, and puts that copy ahead of the
framework's libraries, so #include <WebServer.h> finds it first. Every patch
replaces exact text of the pristine source a fixed number of times, and the
build FAILS if that count is off (the core changed: re-check the patches). The
copy's WebServer.h is stamped with ESP32_MIDI_WIFI_WEBSERVER_PATCHSET, and
src/web_ui.cpp refuses to compile without the matching stamp, so the firmware
can never be built against the unpatched library. Bump PATCHSET (and that
check) whenever a patch changes.
"""

import os
import sys

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

PATCHSET = 1

FRAMEWORK = env.PioPlatform().get_package_dir("framework-arduinoespressif32")  # noqa: F821
SRC_LIB = os.path.join(FRAMEWORK, "libraries", "WebServer")
STORE = os.path.join(env.subst("$BUILD_DIR"), "patched_libs")  # noqa: F821
DST_LIB = os.path.join(STORE, "WebServer")

# (name, old text, new text, how many times old must occur). Applied in order,
# each to the result of the one before.
PATCHES = {
    "src/Parsing.cpp": [
        (
            "helpers",
            """static const char filename[] PROGMEM = "filename";
""",
            """static const char filename[] PROGMEM = "filename";

// [patched:deadline] How long reading one request may take, from its first
// byte to the end of its body -- except an upload the handler allowed to take
// longer (allowLongUpload()). Below the board's 30 s loop watchdog.
#define HTTP_MAX_REQUEST_TIME 20000

static bool beforeDeadline(unsigned long start)
{
  return millis() - start < HTTP_MAX_REQUEST_TIME;
}

// Stream::readStringUntil() under the deadline: everything up to the
// terminator (which is dropped), or what came before 1 s without data (Stream's
// default timeout) or the client hanging up. A line stops growing at 2 kB; the
// rest of it is read and dropped.
static String readLineBefore(WiFiClient& client, char terminator, unsigned long start)
{
  String line;
  unsigned long last = millis();
  while (beforeDeadline(start)) {
    int c = client.read();
    if (c < 0) {
      if (!client.connected() || millis() - last >= 1000) break;
      delay(1);
      continue;
    }
    last = millis();
    if (c == terminator) break;
    if (line.length() < 2048) line += (char) c;
  }
  return line;
}

// True once the client has sent more; false if it hung up, sent nothing for
// HTTP_MAX_POST_WAIT, or the request ran out of time. Until its closing
// boundary, a multipart body always has more to come.
static bool waitForFormData(WiFiClient& client, unsigned long start)
{
  const unsigned long t = millis();
  while (!client.available() && client.connected() && millis() - t < HTTP_MAX_POST_WAIT &&
         beforeDeadline(start))
    delay(2);
  return client.available() > 0 && beforeDeadline(start);
}
""",
            1,
        ),
        (
            "deadline: form body",
            """static char* readBytesWithTimeout(WiFiClient& client, size_t maxLength, size_t& dataLength, int timeout_ms)
{
  char *buf = nullptr;
  dataLength = 0;
  while (dataLength < maxLength) {
    int tries = timeout_ms;
    size_t newLength;
    while (!(newLength = client.available()) && tries--) delay(1);
""",
            """static char* readBytesWithTimeout(WiFiClient& client, size_t maxLength, size_t& dataLength, int timeout_ms, unsigned long start)
{
  char *buf = nullptr;
  dataLength = 0;
  // [patched:deadline] the request's deadline as well as the per-chunk wait
  while (dataLength < maxLength && beforeDeadline(start)) {
    int tries = timeout_ms;
    size_t newLength;
    while (!(newLength = client.available()) && tries-- && beforeDeadline(start)) delay(1);
""",
            1,
        ),
        (
            "deadline: form body call",
            """readBytesWithTimeout(client, _clientContentLength, plainLength, HTTP_MAX_POST_WAIT);""",
            """readBytesWithTimeout(client, _clientContentLength, plainLength, HTTP_MAX_POST_WAIT, _reqStart);""",
            1,
        ),
        (
            "deadline: start",
            """bool WebServer::_parseRequest(WiFiClient& client) {
  // Read the first line of HTTP request
""",
            """bool WebServer::_parseRequest(WiFiClient& client) {
  // [patched:deadline] every read below stops HTTP_MAX_REQUEST_TIME from here
  _reqStart = millis();
  _longUpload = false;
  _uploadAbort = false;
  // Read the first line of HTTP request
""",
            1,
        ),
        (
            "deadline: upload",
            """int WebServer::_uploadReadByte(WiFiClient& client) {
  int res = client.read();

  if (res < 0) {
    while(!client.available() && client.connected())
      delay(2);
""",
            """int WebServer::_uploadReadByte(WiFiClient& client) {
  // [patched:deadline] stop at the request's deadline, unless the handler
  // allowed a long upload, or once the handler has refused the upload
  if (_uploadAbort || (!_longUpload && !beforeDeadline(_reqStart))) return -1;
  int res = client.read();

  if (res < 0) {
    // [patched:deadline] and give up after HTTP_MAX_POST_WAIT of silence
    // instead of waiting for as long as the client keeps the connection open
    const unsigned long start = millis();
    while(!client.available() && client.connected() && millis() - start < HTTP_MAX_POST_WAIT)
      delay(2);
""",
            1,
        ),
        (
            "file part flag",
            """    _postArgsLen = 0;
    while(1){
""",
            """    _postArgsLen = 0;
    bool sawFile = false;  // [patched:deadline] a file part has started
    while(1){
""",
            1,
        ),
        (
            "parts wait",
            """      bool argIsFile = false;

      line = client.readStringUntil('\\r');
""",
            """      bool argIsFile = false;

      // [patched:deadline] the next part must follow: stop when the client has
      // hung up, gone quiet or run out of time instead of looping here forever
      if (!waitForFormData(client, _reqStart)) return sawFile ? _parseFormUploadAborted() : false;
      line = client.readStringUntil('\\r');
""",
            1,
        ),
        (
            "field wait",
            """          if (!argIsFile){
            while(1){
              line = client.readStringUntil('\\r');
""",
            """          if (!argIsFile){
            while(1){
              // [patched:deadline] the same inside a field's value, which also
              // grew by a newline per empty read until memory ran out
              if (!waitForFormData(client, _reqStart)) return sawFile ? _parseFormUploadAborted() : false;
              line = client.readStringUntil('\\r');
""",
            1,
        ),
        (
            "too many args",
            """              log_e("Too many PostArgs (max: %d) in request.", WEBSERVER_MAX_POST_ARGS);
              return false;
""",
            """              log_e("Too many PostArgs (max: %d) in request.", WEBSERVER_MAX_POST_ARGS);
              // [patched:deadline] an upload handler hears of it too
              return sawFile ? _parseFormUploadAborted() : false;
""",
            1,
        ),
        (
            "file part start",
            """            _currentUpload.reset(new HTTPUpload());
""",
            """            _currentUpload.reset(new HTTPUpload());
            sawFile = true;  // [patched:deadline]
""",
            1,
        ),
        # Last: every line read in the file, the ones above included.
        ("lines: CR", "client.readStringUntil('\\r')", "readLineBefore(client, '\\r', _reqStart)", 9),
        ("lines: LF", "client.readStringUntil('\\n')", "readLineBefore(client, '\\n', _reqStart)", 9),
    ],
    "src/WebServer.h": [
        (
            "stamp",
            """#ifndef WEBSERVER_H
#define WEBSERVER_H
""",
            """#ifndef WEBSERVER_H
#define WEBSERVER_H
#define ESP32_MIDI_WIFI_WEBSERVER_PATCHSET %d  // written by tools/patch_webserver.py
""" % PATCHSET,
            1,
        ),
        (
            "upload control",
            """  virtual WiFiClient client() { return _currentClient; }
""",
            """  virtual WiFiClient client() { return _currentClient; }
  // [patched:deadline] For an upload handler: lift the request's time limit
  // for this upload (a firmware image on a slow link), or stop reading it
  // because the handler has already answered.
  void allowLongUpload() { _longUpload = true; }
  void abortUpload() { _uploadAbort = true; }
""",
            1,
        ),
        (
            "deadline state",
            """  int _uploadReadByte(WiFiClient& client);
""",
            """  int _uploadReadByte(WiFiClient& client);
  unsigned long _reqStart = 0;  // [patched:deadline] when this request began
  bool _longUpload = false;
  bool _uploadAbort = false;
""",
            1,
        ),
    ],
}


def fail(msg):
    sys.stderr.write("\npatch_webserver.py: %s\n" % msg)
    env.Exit(1)  # noqa: F821


def write_if_changed(path, data):
    if os.path.exists(path):
        with open(path, "rb") as fh:
            if fh.read() == data:
                return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data)
    return True


def main():
    if not os.path.isfile(os.path.join(SRC_LIB, "src", "WebServer.h")):
        fail("no WebServer library at %s" % SRC_LIB)
    wanted = {"library.properties"}
    for root, _, files in os.walk(os.path.join(SRC_LIB, "src")):
        for name in files:
            wanted.add(os.path.relpath(os.path.join(root, name), SRC_LIB).replace("\\", "/"))
    changed = 0
    for rel in sorted(wanted):
        with open(os.path.join(SRC_LIB, rel), "rb") as fh:
            data = fh.read()
        if rel in PATCHES:
            # The core's sources use LF; compare and patch on that form.
            text = data.decode("utf-8").replace("\r\n", "\n")
            for name, old, new, times in PATCHES[rel]:
                n = text.count(old)
                if n != times:
                    fail("patch '%s': expected its source text %d time(s) in\n  %s\n"
                         "found %d. The core changed -- re-check the patch before\n"
                         "building against it." % (name, times, os.path.join(SRC_LIB, rel), n))
                text = text.replace(old, new)
            data = text.encode("utf-8")
        if write_if_changed(os.path.join(DST_LIB, rel), data):
            changed += 1
    # Drop anything a different core version left behind.
    for root, _, files in os.walk(DST_LIB):
        for name in files:
            rel = os.path.relpath(os.path.join(root, name), DST_LIB).replace("\\", "/")
            if rel not in wanted:
                os.remove(os.path.join(root, name))
    if changed:
        print("patch_webserver.py: patched copy of WebServer written (%d files)" % changed)
    # Library storages are searched in order and the framework's come last, so
    # this copy is the one #include <WebServer.h> resolves to.
    env.Prepend(LIBSOURCE_DIRS=[STORE])  # noqa: F821


main()
