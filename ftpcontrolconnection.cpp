#include "ftpcontrolconnection.h"
#include "ftplistcommand.h"
#include "ftpretrcommand.h"
#include "ftpstorcommand.h"
#include "sslserver.h"
#include "passivedataconnection.h"

#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include <QStringList>
#include <QDir>
#include <QEventLoop>
#include <QDebug>
#include <QTimer>
#include <QSslSocket>

FtpControlConnection::FtpControlConnection(QObject *parent, QSslSocket *socket, const QString &rootPath, const QString &userName, const QString &password, bool readOnly) :
    QObject(parent)
{
    this->socket = socket;
    this->userName = userName;
    this->password = password;
    this->rootPath = rootPath;
    this->readOnly = readOnly;
    isLoggedIn = false;
    encryptDataConnection = false;
    socket->setParent(this);
    connect(socket, SIGNAL(readyRead()), this, SLOT(acceptNewData()));
    connect(socket, SIGNAL(disconnected()), this, SLOT(deleteLater()));
    currentDirectory = "/";
    dataConnection = new PassiveDataConnection(this);
    reply(220, "Welcome to QFtpServer.");
}

FtpControlConnection::~FtpControlConnection()
{
}

void FtpControlConnection::acceptNewData()
{
    if (!socket->canReadLine()) {
        return;
    }

    // Note how we execute only one line, and use QTimer::singleShot, instead
    // of using a for-loop until no more lines are available. This is done
    // so we don't block the event loop for a long time.
    processCommand(QString::fromUtf8(socket->readLine()).trimmed());
    QTimer::singleShot(0, this, SLOT(acceptNewData()));
}

void FtpControlConnection::disconnectFromHost()
{
    socket->disconnectFromHost();
}

QString FtpControlConnection::stripFlagL(const QString &fileName)
{
    QString a = fileName.toUpper();
    if (a == "-L") {
        return "";
    }
    if (a.startsWith("-L ")) {
        return fileName.mid(3);
    }
    return fileName;
}

void FtpControlConnection::parseCommand(const QString &entireCommand, QString *command, QString *commandParameters)
{
    // Split parameters and command.
    int pos = entireCommand.indexOf(' ');
    if (-1 != pos) {
        *command = entireCommand.left(pos).trimmed().toUpper();
        *commandParameters = entireCommand.mid(pos+1).trimmed();
    } else {
        *command = entireCommand.trimmed().toUpper();
    }
}

QString FtpControlConnection::toLocalPath(const QString &fileName) const
{
    QString localPath = fileName;

    // Some FTP clients send backslashes.
    localPath.replace('\\', '/');

    // If this is a relative path, we prepend the current directory.
    if (!localPath.startsWith('/')) {
        localPath = currentDirectory + '/' + localPath;
    }

    // Evaluate all the ".." and ".", "/path/././to/dir/../.." becomes "/path".
    QStringList components;
    foreach (const QString &component, localPath.split('/', QString::SkipEmptyParts)) {
        if (component == "..") {
            if (!components.isEmpty()) {
                components.pop_back();
            }
        } else if (component != ".") {
            components += component;
        }
    }

    // Prepend the root path.
    localPath = QDir::cleanPath(rootPath + '/' + components.join("/"));

    qDebug() << "FtpControlConnection::toLocalPath" << fileName << "->" << localPath;
    return localPath;
}

void FtpControlConnection::reply(int code, const QString &details)
{
    qDebug() << "FtpControlConnection::reply" << code << details;

    if (details.isEmpty()) {
        Q_ASSERT(false);
        socket->write(QString("%1 Comment.\r\n").arg(code).toUtf8());
    } else {
        socket->write(QString("%1 %2\r\n").arg(code).arg(details).toUtf8());
    }
}

void FtpControlConnection::processCommand(const QString &entireCommand)
{
    qDebug() << "FtpControlConnection::processCommand" << entireCommand;

    QString command;
    QString commandParameters;
    parseCommand(entireCommand, &command, &commandParameters);

    if ("USER" == command) {
        reply(331, "User name OK, need password.");
    } else if ("PASS" == command) {
        pass(commandParameters);
    } else if ("QUIT" == command) {
        quit();
    } else if ("AUTH" == command && "TLS" == commandParameters.toUpper()) {
        auth();
    } else if ("FEAT" == command) {
        feat();
    } else if ("PWD" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else {
            reply(227, '"' + currentDirectory + '"');
        }
    } else if ("CWD" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else {
            cwd(commandParameters);
        }
    } else if ("TYPE" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else {
            reply(200, "Command okay.");
        }
    } else if ("PASV" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else {
            pasv();
        }
    } else if ("LIST" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else {
            list(toLocalPath(stripFlagL(commandParameters)), false);
        }
    } else if ("RETR" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else {
            retr(toLocalPath(commandParameters));
        }
    } else if ("REST" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else {
            reply(350, "Requested file action pending further information.");
        }
    } else if ("NLST" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else {
            list(toLocalPath(stripFlagL(commandParameters)), true);
        }
    } else if ("SIZE" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else {
            size(toLocalPath(commandParameters));
        }
    } else if ("SYST" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else {
            reply(215, "UNIX");
        }
    } else if ("PROT" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else {
            prot(commandParameters.toUpper());
        }
    } else if ("CDUP" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else {
            cdup();
        }
    } else if ("OPTS" == command && "UTF8 ON" == commandParameters.toUpper()) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else {
            reply(200, "Command okay.");
        }
    } else if ("PBSZ" == command && "0" == commandParameters.toUpper()) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else {
            reply(200, "Command okay.");
        }
    } else if ("NOOP" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else {
            reply(200, "Command okay.");
        }
    } else if ("STOR" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else if (readOnly) {
            reply(550, "Can't do that in read-only mode.");
        } else {
            stor(toLocalPath(commandParameters));
        }
    } else if ("MKD" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else if (readOnly) {
            reply(550, "Can't do that in read-only mode.");
        } else {
            mkd(toLocalPath(commandParameters));
        }
    } else if ("RMD" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else if (readOnly) {
            reply(550, "Can't do that in read-only mode.");
        } else {
            rmd(toLocalPath(commandParameters));
        }
    } else if ("DELE" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else if (readOnly) {
            reply(550, "Can't do that in read-only mode.");
        } else {
            dele(toLocalPath(commandParameters));
        }
    } else if ("RNFR" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else if (readOnly) {
            reply(550, "Can't do that in read-only mode.");
        } else {
            reply(350, "Requested file action pending further information.");
        }
    } else if ("RNTO" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else if (readOnly) {
            reply(550, "Can't do that in read-only mode.");
        } else {
            rnto(toLocalPath(commandParameters));
        }
    } else if ("APPE" == command) {
        if (!isLoggedIn) {
            reply(530, "You must log in first.");
        } else if (readOnly) {
            reply(550, "Can't do that in read-only mode.");
        } else {
            stor(toLocalPath(commandParameters), true);
        }
    } else {
        reply(502, "Command not implemented.");
    }

    lastProcessedCommand = entireCommand;
}

void FtpControlConnection::startOrScheduleCommand(FtpCommand *ftpCommand)
{
    connect(ftpCommand, SIGNAL(reply(int,QString)), this, SLOT(reply(int,QString)));

    if (!dataConnection->setFtpCommand(ftpCommand)) {
        delete ftpCommand;
        reply(425, "Can't open data connection.");
        return;
    }
}

void FtpControlConnection::pasv()
{
    int port = dataConnection->listen(encryptDataConnection);
    reply(227, QString("Entering Passive Mode. %1,%2,%3").arg(socket->localAddress().toString().replace('.',',')).arg(port/256).arg(port%256));
}

void FtpControlConnection::list(const QString &dir, bool nameListOnly)
{
    startOrScheduleCommand(new FtpListCommand(this, dir, nameListOnly));
}

void FtpControlConnection::retr(const QString &fileName)
{
    startOrScheduleCommand(new FtpRetrCommand(this, fileName, seekTo()));
}

void FtpControlConnection::stor(const QString &fileName, bool appendMode)
{
    startOrScheduleCommand(new FtpStorCommand(this, fileName, appendMode, seekTo()));
}

void FtpControlConnection::cwd(const QString &dir)
{
    QFileInfo fi(toLocalPath(dir));
    if (fi.exists() && fi.isDir()) {
        QFileInfo fi(dir);
        if (fi.isAbsolute()) {
            currentDirectory = QDir::cleanPath(dir);
        } else {
            currentDirectory = QDir::cleanPath(currentDirectory + '/' + dir);
        }
        reply(250, "Requested file action okay, completed.");
    } else {
        reply(550, "Requested action not taken; file unavailable.");
    }
}

void FtpControlConnection::mkd(const QString &dir)
{
    if (QDir().mkdir(dir)) {
        reply(257, '"' + dir + '"' + " created.");
    } else {
        reply(550, "Requested action not taken; file unavailable.");
    }
}

void FtpControlConnection::rmd(const QString &dir)
{
    if (QDir().rmdir(dir)) {
        reply(250, "Requested file action okay, completed.");
    } else {
        reply(550, "Requested action not taken; file unavailable.");
    }
}

void FtpControlConnection::dele(const QString &fileName)
{
    if (QDir().remove(fileName)) {
        reply(250, "Requested file action okay, completed.");
    } else {
        reply(550, "Requested action not taken; file unavailable.");
    }
}

void FtpControlConnection::rnto(const QString &fileName)
{
    QString command;
    QString commandParameters;
    parseCommand(lastProcessedCommand, &command, &commandParameters);
    if ("RNFR" == command && QDir().rename(toLocalPath(commandParameters), fileName)) {
        reply(250, "Requested file action okay, completed.");
    } else {
        reply(550, "Requested action not taken; file unavailable.");
    }
}

void FtpControlConnection::quit()
{
    reply(221, "Quitting...");
    // If we have a running download or upload, we will wait until it's
    // finished before closing the control connection.
    if (dataConnection->ftpCommand()) {
        connect(dataConnection->ftpCommand(), SIGNAL(destroyed()), this, SLOT(disconnectFromHost()));
    } else {
        disconnectFromHost();
    }
}

void FtpControlConnection::size(const QString &fileName)
{
    QFileInfo fi(fileName);
    if (!fi.exists() || fi.isDir()) {
        reply(550, "Requested action not taken; file unavailable.");
    } else {
        reply(213, QString("%1").arg(fi.size()));
    }
}

void FtpControlConnection::pass(const QString &password)
{
    QString command;
    QString commandParameters;
    parseCommand(lastProcessedCommand, &command, &commandParameters);
    if (this->password.isEmpty() || ("USER" == command && this->userName == commandParameters && this->password == password)) {
        reply(230, "You are logged in.");
        isLoggedIn = true;
    } else {
        reply(530, "User name or password was incorrect.");
    }
}

void FtpControlConnection::auth()
{
    reply(234, "Initializing SSL connection.");
    SslServer::setLocalCertificateAndPrivateKey(socket);
    socket->startServerEncryption();
}

void FtpControlConnection::prot(const QString &protectionLevel)
{
    if ("C" == protectionLevel) {
        encryptDataConnection = false;
    } else if ("P" == protectionLevel) {
        encryptDataConnection = true;
    } else {
        reply(502, "Command not implemented.");
        return;
    }
    reply(200, "Command okay.");
}

void FtpControlConnection::cdup()
{
    if ("/" == currentDirectory) {
        reply(250, "Requested file action okay, completed.");
    } else {
        cwd("..");
    }
}

void FtpControlConnection::feat()
{
    // We only report that we support UTF8 file names, this is needed because
    // some clients will assume that we use ASCII otherwise, and will not
    // encode the filenames properly.
    socket->write(
        "211-Features:\r\n"
        " UTF8\r\n"
        "211 End\r\n"
    );
}

qint64 FtpControlConnection::seekTo()
{
    qint64 seekTo = 0;
    QString command;
    QString commandParameters;
    parseCommand(lastProcessedCommand, &command, &commandParameters);
    if ("REST" == command) {
        QTextStream(commandParameters.toUtf8()) >> seekTo;
    }
    return seekTo;
}
