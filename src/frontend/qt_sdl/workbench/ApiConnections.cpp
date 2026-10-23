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

#include "ApiConnections.h"

ApiConnections::Conn* ApiConnections::Add(QTcpSocket* s, bool control)
{
    Conn* c = new Conn;
    c->Sock = s;
    c->Control = control;
    Conns.push_back(c);
    connect(s, &QTcpSocket::disconnected, this, [this, s]()
    {
        for (size_t i = 0; i < Conns.size(); i++)
            if (Conns[i]->Sock == s) { delete Conns[i]; Conns.erase(Conns.begin() + i); break; }
        s->deleteLater();
    });
    return c;
}

ApiConnections::Conn* ApiConnections::Find(QTcpSocket* s) const
{
    for (Conn* c : Conns) if (c->Sock == s) return c;
    return nullptr;
}

void ApiConnections::CloseAll()
{
    // disconnectFromHost emits disconnected at once when nothing is waiting to be written, and that
    // handler deletes the connection too: take the list and detach the handler first
    std::vector<Conn*> conns;
    conns.swap(Conns);
    for (Conn* c : conns)
    {
        if (c->Sock)
        {
            c->Sock->disconnect(this);
            c->Sock->disconnectFromHost();
        }
        delete c;
    }
}
