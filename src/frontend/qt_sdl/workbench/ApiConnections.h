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

#ifndef WB_APICONNECTIONS_H
#define WB_APICONNECTIONS_H

#include <map>
#include <memory>
#include <vector>
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTcpSocket>

struct ApiScan;

// The API server's open connections. Each is deleted once: when its socket disconnects, or by CloseAll.
class ApiConnections : public QObject
{
public:
    struct Conn
    {
        Conn() { Live++; }
        ~Conn() { Live--; }
        static inline int Live = 0;     // connections not deleted yet

        QPointer<QTcpSocket> Sock;
        bool Control = false;
        bool Authed = false;
        QSet<QString> Events;
        bool AllEvents = false;
        QByteArray Buffer;
        QString AgentName = "agent";    // author of this connection's knowledge-store edits
        QString AgentSession;           // change log tag: name#connection
        // search.scan: value scans narrowed down across calls, the newest 8 kept
        std::map<unsigned, std::shared_ptr<ApiScan>> Scans;
        unsigned NextScan = 1;
    };

    ~ApiConnections() override { CloseAll(); }

    Conn* Add(QTcpSocket* s, bool control);
    Conn* Find(QTcpSocket* s) const;
    const std::vector<Conn*>& List() const { return Conns; }
    // disconnects and deletes every connection
    void CloseAll();

private:
    std::vector<Conn*> Conns;
};

#endif
