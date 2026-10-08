#include "protocol.h"
#include "receiverpage.h"
#include "senderpage.h"

#include <QApplication>
#include <QMainWindow>
#include <QStatusBar>
#include <QTabWidget>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <timeapi.h>
#endif

// Creates the main window with the Receiver and Sender tabs and runs the event loop.
int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    timeBeginPeriod(1);   // 1 ms sleep granularity for the sender's rate pacing
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("UDP Bandwidth Tester"));
    QApplication::setApplicationVersion(QStringLiteral("1.0"));

    qRegisterMetaType<TxStats>("TxStats");
    qRegisterMetaType<RxStats>("RxStats");

    QMainWindow window;
    auto *tabs = new QTabWidget;
    tabs->addTab(new ReceiverPage, QObject::tr("Receiver"));
    tabs->addTab(new SenderPage, QObject::tr("Sender"));
    window.setCentralWidget(tabs);
    window.statusBar()->showMessage(
            QObject::tr("Start the Receiver on one machine, then run the Sender on the other "
                        "(same port, receiver's IP as the target)."));
    window.setWindowTitle(QStringLiteral("UDP Bandwidth Tester"));
    window.resize(1150, 800);
    window.show();

    const int rc = app.exec();

#ifdef Q_OS_WIN
    timeEndPeriod(1);
#endif
    return rc;
}
