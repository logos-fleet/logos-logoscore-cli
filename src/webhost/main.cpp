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
    close: function () { open = false; },
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
    LoggingPage(QWebEngineProfile* profile, QByteArray module)
        : QWebEnginePage(profile), m_module(std::move(module)) {}

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
                m_module.isEmpty() ? "page" : m_module.constData(),
                lineNumber, message.toUtf8().constData(),
                sourceID.toUtf8().constData());
    }

private:
    QByteArray m_module;
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
    parser.addOption(entryOpt);
    parser.addOption(portOpt);
    parser.addOption(moduleOpt);
    parser.addHelpOption();
    parser.process(app);

    const QString entry = parser.value(entryOpt);
    const quint16 port = static_cast<quint16>(parser.value(portOpt).toUShort());
    const QByteArray module = parser.value(moduleOpt).toUtf8();
    if (entry.isEmpty() || port == 0) {
        fprintf(stderr, "logoscore-webhost: --entry and --port are required\n");
        return 2;
    }

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

    // A profile of its own, off the record. Two web modules are two processes
    // and therefore two profiles, so nothing a page stores is reachable from
    // another module's page -- the storage half of the identity separation the
    // container gets structurally from one channel per view.
    QWebEngineProfile profile;
    LoggingPage page(&profile, module);
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

    // A renderer crash is the page's death, and the host has to see it. Quitting
    // closes the socket, which is exactly the EOF the daemon's pump reports as
    // "the module lost its page".
    QObject::connect(&page, &QWebEnginePage::renderProcessTerminated,
                     [&app, module](QWebEnginePage::RenderProcessTerminationStatus status,
                                    int exitCode) {
                         fprintf(stderr,
                                 "logoscore-webhost: the renderer for %s terminated "
                                 "(status %d, exit %d)\n",
                                 module.isEmpty() ? "the page" : module.constData(),
                                 static_cast<int>(status), exitCode);
                         app.quit();
                     });

    QObject::connect(&page, &QWebEnginePage::loadFinished, [module](bool ok) {
        if (!ok)
            fprintf(stderr, "logoscore-webhost: %s failed to load its entry document\n",
                    module.isEmpty() ? "the page" : module.constData());
    });

    page.load(QUrl::fromLocalFile(entry));
    return app.exec();
}

#include "main.moc"
