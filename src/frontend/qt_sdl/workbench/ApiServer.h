/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef WB_APISERVER_H
#define WB_APISERVER_H

#include <map>
#include <memory>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include "ApiConnections.h"

class QTcpServer;
class QTcpSocket;
class WBHost;
struct ApiScan;

// JSON-RPC 2.0 over TCP on 127.0.0.1, one JSON object per line.
// Two listeners: the read endpoint (queries) and the control endpoint (state changes).
class ApiServer : public QObject
{
    Q_OBJECT
public:
    ApiServer(WBHost* host, QObject* parent = nullptr);
    ~ApiServer();

    bool Start(int port, bool memoryWrite);
    void Stop();
    bool Running() const { return ReadServer != nullptr; }
    int ReadPort() const { return RPort; }
    int ControlPort() const { return CPort; }
    QString Token() const { return Tok; }
    QString InfoFile() const { return InfoPath; }
    bool MemoryWriteEnabled() const { return MemWrite; }
    void SetMemoryWrite(bool on) { MemWrite = on; }

    void Broadcast(const QString& event, const QJsonObject& params);

private:
    using Conn = ApiConnections::Conn;

    void OnNewConnection(bool control);
    void OnReadyRead(QTcpSocket* s);
    void Handle(Conn* c, const QJsonValue& id, const QString& method, const QJsonObject& params);
    void Reply(QPointer<QTcpSocket> s, const QJsonValue& id, const QJsonObject& result);
    void Error(QPointer<QTcpSocket> s, const QJsonValue& id, int code, const QString& msg);
    int ConnCounter = 0;

    WBHost* Host;
    QTcpServer* ReadServer = nullptr;
    QTcpServer* ControlServer = nullptr;
    int RPort = 0, CPort = 0;
    QString Tok;
    QString InfoPath;
    bool MemWrite = false;
    ApiConnections Conns;
};

#endif
