// logoscore-webhost -- ONE PAGE, ONE SOCKET.
//
// The desktop end of the Web container: liblogos names no browser (it is linked
// into a headless CLI, a desktop shell and a static iOS core, and exactly one of
// those can host a Chromium), so the browser lives out here, in a process of its
// own, and this binary is the whole of it.
//
// What it does is deliberately small. It is NOT a Logos host: it never decodes a
// protocol message, never knows a module's name beyond a log line, and holds no
// tokens. It moves opaque JSON texts between a socket and a page:
//
//     daemon ── loopback socket ──> this process ── QWebChannel ──> the page
//
// with one text per line in both directions. A web-transport message is compact
// JSON and JSON escapes every newline inside a string, so a literal '\n' can
// only ever be the separator.
//
// The page sees none of that. It is handed `window.logosChannelReady`, a Promise
// of a logos-js-sdk channel ({send, setReceiver, close, isOpen}) -- the same five
// methods `messagePortChannel()` produces -- so a module page written against the
// browser SDK runs here unmodified and would run against a MessagePort, a
// worker, or a phone's webview just as well.
//
// Exit codes: 2 bad arguments, 3 the daemon's socket was unreachable, 4 this Qt
// build ships no qwebchannel.js. A page that fails to LOAD is not an exit code:
// the container settles that by asking the page for its module and getting
// nothing, which is the same verdict for a page that loaded and published
// nothing.

#include <QtWebEngineQuick/QtWebEngineQuick>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFile>
#include <QGuiApplication>
#include <QStringList>
#include <QHostAddress>
#include <QTcpSocket>
#include <QUrl>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QDir>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>

#include <cstdio>

namespace {

// The object QWebChannel publishes to the page. Two directions, no policy.
//
// IT BUFFERS THE HOST-TO-PAGE DIRECTION UNTIL THE PAGE SAYS IT IS LISTENING,
// and that is not an optimisation. A QWebChannel signal emitted before the JS
// client has finished its handshake reaches nobody -- there is no subscriber to
// deliver it to and no queue behind it -- while the host starts talking the
// moment its socket is up. The first thing the core sends after publishing a
// web module is the wildcard event SUBSCRIBE (ModuleProxy installs the event
// listener from its constructor, inside registerObject), so without this the
// page never learns anyone is listening and every event it emits goes nowhere.
// The symptom is not an error: calls work, introspection works, and events are
// silently missing.
class Bridge : public QObject {
    Q_OBJECT
public:
    explicit Bridge(QObject* parent = nullptr) : QObject(parent) {}

    // Called from the socket side, on the host's own thread.
    void deliverToPage(const QString& text)
    {
        if (!m_pageReady) { m_backlog.append(text); return; }
        emit toPage(text);
    }

public slots:
    // Called BY THE PAGE. A slot rather than a Q_INVOKABLE only because
    // QWebChannel exposes both and this reads as what it is: an inbox.
    void toHost(const QString& text) { emit fromPage(text); }

    // Called BY THE PAGE when it closes its end of the channel.
    //
    // A PAGE CLOSING ITS CHANNEL IS A MODULE FAILURE, and this slot is what
    // makes it one. Until it existed the shim's close() only flipped a flag
    // inside the page: the host kept the socket open, the container went on
    // believing the module was there, and the next call into it burned the full
    // introspection timeout before failing for the wrong reason.
    //
    // Two things close a channel, and both are the page saying it can no longer
    // serve: a wasm image that trapped (the `web` variant's loader page reports
    // a Rust panic exactly this way -- slice 26) and logos-js-sdk's own peer
    // failing a connection it cannot decode. So the answer to both is the same,
    // and it is the answer this process already has for a dead renderer: quit,
    // which closes the socket, which is the EOF the daemon's pump reports as
    // "the module lost its page".
    void closed() { emit pageClosed(); }

    // Called BY THE PAGE, once, when its end of the channel is connected.
    void ready()
    {
        if (m_pageReady) return;
        m_pageReady = true;
        const QStringList backlog = m_backlog;
        m_backlog.clear();
        for (const QString& text : backlog) emit toPage(text);
    }

signals:
    void toPage(const QString& text);
    void fromPage(const QString& text);
    void pageClosed();

private:
    bool m_pageReady = false;
    QStringList m_backlog;
};

// The shim, injected at document creation in the main world.
//
// It runs BEFORE the page's own scripts, which is what lets a module page do
//     const channel = await window.logosChannelReady;
// at the top of its first <script> and be sure the promise is there. The
// QWebChannel handshake itself is asynchronous, so the promise -- not the
// channel -- is what can be published synchronously.
//
// Messages that arrive before the page installs a receiver are QUEUED rather
// than dropped: the host's transport starts talking as soon as it is connected,
// and a page whose script has not run yet has not lost the conversation, it just
// has not joined it.
const char* kChannelShim = R"JS(
(function () {
  var pending = [];
  var receiver = null;
  var open = true;
  var bridge = null;

  function deliver(text) {
    if (receiver) {
      // Never inline: the SDK's peer writes with its own registry lock held,
      // and a channel that called back synchronously would re-enter it.
      var cb = receiver;
      setTimeout(function () { cb(text); }, 0);
    } else {
      pending.push(text);
    }
  }

  var channel = {
    send: function (text) {
      if (!open || !bridge) return false;
      bridge.toHost(String(text));
      return true;
    },
    setReceiver: function (fn) {
      receiver = fn || null;
      if (!receiver) return;
      var queued = pending;
      pending = [];
      queued.forEach(deliver);
    },
    close: function () {
      if (!open) return;
      open = false;
      // TELL THE HOST. A channel the page has closed is a module that has
      // stopped serving, and the host cannot see that from its own end: the
      // socket is still up and the page is still loaded. See Bridge::closed().
      if (bridge) bridge.closed();
    },
    isOpen: function () { return open; }
  };

  window.logosChannelReady = new Promise(function (resolve, reject) {
    function build() {
      try {
        new QWebChannel(qt.webChannelTransport, function (ch) {
          bridge = ch.objects.logos;
          bridge.toPage.connect(deliver);
          // Only now can a signal from the host reach this page, so only now
          // may the host stop holding them. See Bridge::ready().
          bridge.ready();
          resolve(channel);
        });
      } catch (e) {
        reject(e);
      }
    }
    // qt.webChannelTransport is injected by WebEngine's own document-creation
    // script. Ordering between two document-creation scripts is not something
    // to rely on, so this waits when it has to.
    if (typeof qt !== 'undefined' && qt.webChannelTransport) build();
    else document.addEventListener('DOMContentLoaded', build);
  });
})();
)JS";

// A page whose script throws is the hardest failure to diagnose from the
// outside: the container sees "the page never published a module" and nothing
// else, because a page that crashed on line one is indistinguishable from one
// that simply has no module in it. So the console goes to stderr, which the
// daemon that spawned this process already collects.
class LoggingPage : public QWebEnginePage {
public:
    // `label` is what these lines call the page — see `pageLabel` in main(),
    // which is the one place the fallback for a missing `--module` is chosen.
    LoggingPage(QWebEngineProfile* profile, QByteArray label)
        : QWebEnginePage(profile), m_label(std::move(label)) {}

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
                                  const QString& message,
                                  int lineNumber,
                                  const QString& sourceID) override
    {
        static const char* kLevels[] = { "info", "warning", "error" };
        const int i = static_cast<int>(level);
        fprintf(stderr, "logoscore-webhost: [%s] %s:%d %s (%s)\n",
                (i >= 0 && i < 3) ? kLevels[i] : "?",
                m_label.constData(),
                lineNumber, message.toUtf8().constData(),
                sourceID.toUtf8().constData());
    }

private:
    QByteArray m_label;
};

QString qtResource(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

} // namespace

int main(int argc, char** argv)
{
    // Before the application object, as QtWebEngine requires.
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("logoscore-webhost"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Runs one Logos web module's page and relays the web "
                       "transport to the logoscore daemon."));
    QCommandLineOption entryOpt(QStringLiteral("entry"),
                                QStringLiteral("The module's entry document."),
                                QStringLiteral("path"));
    QCommandLineOption portOpt(QStringLiteral("port"),
                               QStringLiteral("The daemon's loopback port."),
                               QStringLiteral("port"));
    QCommandLineOption moduleOpt(QStringLiteral("module"),
                                 QStringLiteral("Module name, for log lines only."),
                                 QStringLiteral("name"));
    QCommandLineOption storageOpt(
        QStringLiteral("storage"),
        QStringLiteral("Where this module's page may store data across runs. "
                       "Omit it and the page runs off the record."),
        QStringLiteral("dir"));
    parser.addOption(entryOpt);
    parser.addOption(portOpt);
    parser.addOption(moduleOpt);
    parser.addOption(storageOpt);
    parser.addHelpOption();
    parser.process(app);

    const QString entry = parser.value(entryOpt);
    const quint16 port = static_cast<quint16>(parser.value(portOpt).toUShort());
    const QByteArray module = parser.value(moduleOpt).toUtf8();
    const QString storageDir = parser.value(storageOpt);
    if (entry.isEmpty() || port == 0) {
        fprintf(stderr, "logoscore-webhost: --entry and --port are required\n");
        return 2;
    }

    // What every log line below calls this page. `--module` is for diagnostics
    // only and a caller may omit it, so the fallback is chosen once here rather
    // than repeated at each fprintf. A QByteArray so the lambdas that use it
    // can capture it by value and own their own copy.
    const QByteArray pageLabel = module.isEmpty() ? QByteArrayLiteral("the page") : module;

    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(10000)) {
        fprintf(stderr, "logoscore-webhost: cannot reach the daemon on 127.0.0.1:%u\n",
                static_cast<unsigned>(port));
        return 3;
    }

    const QString webChannelJs = qtResource(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
    if (webChannelJs.isEmpty()) {
        fprintf(stderr, "logoscore-webhost: qwebchannel.js is not in this Qt build\n");
        return 4;
    }

    Bridge bridge;
    QWebChannel channel;
    channel.registerObject(QStringLiteral("logos"), &bridge);

    // A PROFILE OF ITS OWN, and -- when the daemon gave us somewhere to put it
    // -- a PERSISTENT one.
    //
    // Two web modules are two processes and therefore two profiles, so nothing
    // a page stores is reachable from another module's page: that is the
    // storage half of the identity separation the container gets structurally
    // from one channel per view, and it survives this change because the
    // directory below is the module's own instance-persistence directory.
    //
    // Persistence is not optional for a real module, and the way it failed is
    // the problem. A default-constructed QWebEngineProfile is OFF THE RECORD,
    // which for a wasm module means IDBFS mounts, `commit()` pushes into
    // IndexedDB, the push SUCCEEDS -- and the whole database is in memory and
    // dies with this process. Measured with the keystore's `web` variant: it
    // created a key, encrypted it, reported the write durable, and a fresh
    // daemon listed no accounts. Nothing raised an error at any point, which is
    // exactly the failure this slice's storage barrier exists to remove one
    // level down.
    //
    // The storage NAME is the module's, and the PATH is the directory the host
    // already assigns that module. Qt keys an off-the-record profile off the
    // empty name, so the name is what makes it persistent at all; the path is
    // what keeps it beside the module's other state instead of in a shared
    // application-data directory.
    const bool offTheRecord = storageDir.isEmpty();
    QWebEngineProfile profile(offTheRecord ? QString()
                                           : QString::fromUtf8(pageLabel));
    if (!offTheRecord) {
        QDir().mkpath(storageDir);
        profile.setPersistentStoragePath(storageDir);
        profile.setCachePath(storageDir + QStringLiteral("/cache"));
        profile.setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
    }
    fprintf(stderr, "logoscore-webhost: %s storage: %s\n", pageLabel.constData(),
            offTheRecord ? "off the record (the daemon named no persistence path)"
                         : qPrintable(storageDir));
    LoggingPage page(&profile, pageLabel);
    page.setWebChannel(&channel);

    QWebEngineScript script;
    script.setName(QStringLiteral("logos-web-channel"));
    script.setInjectionPoint(QWebEngineScript::DocumentCreation);
    script.setWorldId(QWebEngineScript::MainWorld);
    script.setRunsOnSubFrames(false);
    script.setSourceCode(webChannelJs + QString::fromUtf8(kChannelShim));
    page.scripts().insert(script);

    QObject::connect(&bridge, &Bridge::fromPage, [&socket](const QString& text) {
        socket.write(text.toUtf8());
        socket.write("\n");
        socket.flush();
    });

    QByteArray inbox;
    QObject::connect(&socket, &QTcpSocket::readyRead, [&]() {
        inbox += socket.readAll();
        int nl;
        while ((nl = inbox.indexOf('\n')) >= 0) {
            const QByteArray line = inbox.left(nl);
            inbox.remove(0, nl + 1);
            if (!line.isEmpty())
                bridge.deliverToPage(QString::fromUtf8(line));
        }
    });

    // The daemon closing the socket is how a module is unloaded.
    QObject::connect(&socket, &QTcpSocket::disconnected, &app, &QGuiApplication::quit);

    // The page closing its channel is how a module reports that it died. Same
    // exit as a dead renderer, for the same reason: the daemon learns from the
    // socket, and it is the only thing it can learn from.
    QObject::connect(&bridge, &Bridge::pageClosed, [&app, pageLabel]() {
        fprintf(stderr,
                "logoscore-webhost: %s closed its channel; it is no longer "
                "serving\n", pageLabel.constData());
        app.quit();
    });

    // A renderer crash is the page's death, and the host has to see it. Quitting
    // closes the socket, which is exactly the EOF the daemon's pump reports as
    // "the module lost its page".
    QObject::connect(&page, &QWebEnginePage::renderProcessTerminated,
                     [&app, pageLabel](QWebEnginePage::RenderProcessTerminationStatus status,
                                       int exitCode) {
                         fprintf(stderr,
                                 "logoscore-webhost: the renderer for %s terminated "
                                 "(status %d, exit %d)\n",
                                 pageLabel.constData(),
                                 static_cast<int>(status), exitCode);
                         app.quit();
                     });

    QObject::connect(&page, &QWebEnginePage::loadFinished, [pageLabel](bool ok) {
        if (!ok)
            fprintf(stderr, "logoscore-webhost: %s failed to load its entry document\n",
                    pageLabel.constData());
    });

    page.load(QUrl::fromLocalFile(entry));
    return app.exec();
}

#include "main.moc"
