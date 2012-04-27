#include "InVpn.hpp"
#include "InVpnNode.hpp"
#include <QCoreApplication>
#include <QStringList>
#include <QFile>
#include <QSslConfiguration>
#include <QDateTime>
#include <qendian.h>

InVpn::InVpn() {
	tap = NULL;
	bc_last_id = 0;
	parseCmdLine();

	// initialize DB
	db = QSqlDatabase::addDatabase("QSQLITE");
	db.setDatabaseName(db_path);
	if (!db.open()) {
		qDebug("Could not open database");
		QCoreApplication::exit(1);
		return;
	}

	// initialize SSL
	QFile key_file(key_path);
	if (!key_file.open(QIODevice::ReadOnly)) {
		qDebug("Could not open key file");
		QCoreApplication::exit(1);
		return;
	}
	ssl_key = QSslKey(&key_file, QSsl::Rsa);
	if (ssl_key.isNull()) {
		qDebug("failed to parse key file");
		QCoreApplication::exit(1);
		return;
	}
	key_file.close();
	ssl_cert = QSslCertificate::fromPath(cert_path, QSsl::Pem, QRegExp::FixedString).at(0);
	ssl_ca = QSslCertificate::fromPath(ca_path, QSsl::Pem);

	if (ssl_cert.isNull()) {
		qDebug("failed to parse cert");
		QCoreApplication::exit(1);
		return;
	}
	if (ssl_ca.size() == 0) {
		qDebug("failed to parse CA file");
		QCoreApplication::exit(1);
		return;
	}

	// Set CA list for all future configs
	QSslConfiguration config = QSslConfiguration::defaultConfiguration();
	config.setCaCertificates(ssl_ca);
	config.setLocalCertificate(ssl_cert);
	config.setPrivateKey(ssl_key);
	config.setPeerVerifyMode(QSslSocket::VerifyPeer);
	QSslConfiguration::setDefaultConfiguration(config);

	QString tmpmac = ssl_cert.subjectInfo(QSslCertificate::CommonName);
	mac = QByteArray::fromHex(tmpmac.toLatin1().replace(":",""));

	server = new InVpnSslServer();
	if (!server->listen(QHostAddress::Any, port)) {
		qDebug("failed to listen to net");
		QCoreApplication::exit(1);
		return;
	}

	tap = new QTap("invpn%d", this);
	if (!tap->isValid()) {
		delete tap;
		tap = NULL;
		return;
	}
	tap->setMac(mac);

	connect(server, SIGNAL(ready(QSslSocket*)), this, SLOT(accept(QSslSocket*)));
	connect(tap, SIGNAL(packet(const QByteArray&, const QByteArray&, const QByteArray&)), this, SLOT(packet(const QByteArray&, const QByteArray&, const QByteArray&)));
	connect(&announce_timer, SIGNAL(timeout()), this, SLOT(announce()));
	connect(&connect_timer, SIGNAL(timeout()), this, SLOT(tryConnect()));

	announce_timer.setInterval(10000);
	announce_timer.setSingleShot(false);
	announce_timer.start();
	connect_timer.setInterval(60000);
	connect_timer.setSingleShot(false);
	connect_timer.start();

	qDebug("got interface: %s", qPrintable(tap->getName()));

	tryConnect(); // try to connect to stuff now
}

void InVpn::tryConnect() {
	// we want at least two links established, let's count now!
	int count = 0;

	auto i = nodes.begin();
	while(i != nodes.end()) {
		if (i.value()->isLinked()) count++;
		i++;
	}
	if (count >= 2) return;

	if (init_seed.isNull()) {
//		qDebug("no node to connect to, giving up");
		return;
	}

	// format is either: 127.0.0.1:1234 [::1]:1234
	// Because of the way this works, placing an IPv6 without brackets works too: ::1:1234
	// IPv4 with brackets works too: [127.0.0.1]:1234
	
	int pos = init_seed.indexOf('@');
	if (pos == -1) {
		qDebug("Bad syntax for initial seed, giving up");
		return;
	}
	QString rmac = init_seed.mid(0, pos);
	QString addr = init_seed.mid(pos+1);

	pos = addr.lastIndexOf(':');
	if (pos == -1) {
		qDebug("port missing, giving up");
		return;
	}

	int port = addr.mid(pos+1).toInt();
	QString tip = addr.mid(0, pos);

	if ((tip[0] == '[') && (tip.at(tip.size()-1) == ']')) {
		tip = tip.mid(1, tip.size()-2);
	}
	QHostAddress ip(tip);
	if (ip.isNull()) {
		qDebug("malformed initial seed ip, giving up");
		return;
	}

	qDebug("trying to connect to %s on port %d", qPrintable(ip.toString()), port);

	QSslSocket *s = new QSslSocket(this);
	connect(s, SIGNAL(connected()), s, SLOT(startClientEncryption()));
	connect(s, SIGNAL(sslErrors(const QList<QSslError>&)), this, SLOT(sslErrors(const QList<QSslError>&)));
	connect(s, SIGNAL(encrypted()), this, SLOT(socketReady()));
	connect(s, SIGNAL(disconnected()), this, SLOT(socketLost()));
	connect(s, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(socketError(QAbstractSocket::SocketError)));
	s->connectToHost(ip, port);
	s->setPeerVerifyName(rmac);
}

void InVpn::announce() {
	// broadcast to all peers that we are here
	QByteArray pkt;

	qint64 ts = qToBigEndian(broadcastId());

	pkt.append((char)1); // version
	pkt.append((char*)&ts, 8);
	pkt.append(mac);

	pkt.prepend((char)0);
	quint16 len = pkt.size();
	len = qToBigEndian(len);
	pkt.prepend((char*)&len, 2);

	qDebug("broadcast: %s", pkt.toHex().constData());
	broadcast(pkt);
}

qint64 InVpn::broadcastId() {
	// return a milliseconds unique timestamp, let's hope we won't have a sustained 1000 pkt/sec of broadcast
	qint64 now = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
	if (now <= bc_last_id) {
		bc_last_id++;
		return bc_last_id;
	}
	bc_last_id = now;
	return now;
}

void InVpn::accept(QSslSocket*s) {
	connect(s, SIGNAL(sslErrors(const QList<QSslError>&)), this, SLOT(sslErrors(const QList<QSslError>&)));
	connect(s, SIGNAL(disconnected()), this, SLOT(socketLost()));
	connect(s, SIGNAL(encrypted()), this, SLOT(socketReady()));
	connect(s, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(socketError(QAbstractSocket::SocketError)));
	s->startServerEncryption();
}

void InVpn::sslErrors(const QList<QSslError>&l) {
	qDebug("SSL errors in peer connection:");
	for(int i = 0; i < l.size(); i++) {
		qDebug(" * %s", qPrintable(l.at(i).errorString()));
	}
	QSslSocket *s = qobject_cast<QSslSocket*>(sender());
	if (!s) {
		qDebug("Source was not a QsslSocket? :(");
		return;
	}
	s->deleteLater();
}

void InVpn::socketReady() {
	QSslSocket *s = qobject_cast<QSslSocket*>(sender());
	if (!s) return;

	QSslCertificate p = s->peerCertificate();
	QString tmpmac = p.subjectInfo(QSslCertificate::CommonName); // xx:xx:xx:xx:xx:xx
	QByteArray m = QByteArray::fromHex(tmpmac.toLatin1().replace(":",""));

	if (m == mac) {
		// connected to myself?!
		qDebug("connected to self, closing");
		s->disconnect();
		s->deleteLater();
	}

	// do we know this node ?
	if (!nodes.contains(m)) {
		nodes.insert(m, new InVpnNode(this, m));
		connect(this, SIGNAL(broadcast(const QByteArray&)), nodes.value(m), SLOT(push(const QByteArray&)));
	}
	if (!nodes.value(m)->setLink(s)) {
		// already got a link to that node?
		qDebug("already got a link to this guy, closing it");
		s->disconnect();
		s->deleteLater();
	}
}

void InVpn::socketLost() {
	QSslSocket *s = qobject_cast<QSslSocket*>(sender());
	if (!s) return;

//	QString peer = invpn_socket_name(s);
//	qDebug("lost peer %s", qPrintable(peer));
//
//	peers.remove(peer);
	s->deleteLater();
}

void InVpn::socketError(QAbstractSocket::SocketError) {
	QSslSocket *s = qobject_cast<QSslSocket*>(sender());
	if (!s) return;

	qDebug("error from socket: %s", qPrintable(s->errorString()));
	s->deleteLater();
}

bool InVpn::isValid() {
	if (tap == NULL) return false;
	return true;
}

void InVpn::packet(const QByteArray &src_hw, const QByteArray &dst_hw, const QByteArray &data) {
	if (src_hw != mac) {
		qDebug("dropped packet from wrong mac addr");
		return;
	}
//	qDebug("packet data: [%s] => [%s] %s", src_hw.toHex().constData(), dst_hw.toHex().constData(), data.toHex().constData());

	if (dst_hw == QByteArray(6, '\xff')) {
		// broadcast!
		QByteArray pkt;

		qint64 ts = qToBigEndian(broadcastId());

		pkt.append((char*)&ts, 8);
		pkt.append(src_hw);
		pkt.append(data);

		pkt.prepend((char)0x81); // broadcast
		quint16 len = pkt.size();
		len = qToBigEndian(len);
		pkt.prepend((char*)&len, 2);

		qDebug("broadcast: %s", pkt.toHex().constData());
		broadcast(pkt);
		return;
	}
	if (!routes.contains(dst_hw)) {
		qDebug("Packet to unroutable mac addr %s ignored", dst_hw.toHex().constData());
		return;
	}

	QByteArray pkt;
	pkt.append(dst_hw);
	pkt.append(src_hw);
	pkt.append(data);

	pkt.prepend((char)0x80); // targetted
	quint16 len = pkt.size();
	len = qToBigEndian(len);
	pkt.prepend((char*)&len, 2);

	route(pkt);
//	nodes.value(dst_hw).push(pkt);
}

void InVpn::announcedRoute(const QByteArray &mac, InVpnNode *peer, qint64 stamp, const QByteArray &pkt) {
	if (routes.contains(mac)) {
		if (routes.value(mac).stamp >= stamp) return;
		routes[mac].stamp = stamp;
		routes[mac].peer = peer;
		broadcast(pkt);
		return;
	}
	struct invpn_route_info s;
	s.peer = peer;
	s.stamp = stamp;
	routes.insert(mac, s);
	broadcast(pkt);
}

void InVpn::route(const QByteArray &pkt) {
	if ((unsigned char)pkt.at(2) != 0x80) return; // not a directed packet
	QByteArray dst_mac = pkt.mid(3, 6);
	qDebug("route pkt to %s", dst_mac.toHex().constData());
	if (!routes.contains(dst_mac)) return;
	if (!routes.value(dst_mac).peer) return;
	routes.value(dst_mac).peer->push(pkt);
}

void InVpn::parseCmdLine() {
	// set default settings, then try to parse cmdline
	port = 41744;
	key_path = "conf/client.key";
	cert_path = "conf/client.crt";
	ca_path = "conf/ca.crt";
	db_path = "conf/client.db";

	QStringList cmdline = QCoreApplication::arguments();

	// Why isn't there a cmdline parser included with Qt? ;_;
	for(int i = 1; i < cmdline.size(); i++) {
		QString tmp = cmdline.at(i);
		if (tmp == "-k") {
			key_path = cmdline.at(i+1); i++; continue;
		}
		if (tmp == "-c") {
			cert_path = cmdline.at(i+1); i++; continue;
		}
		if (tmp == "-a") {
			ca_path = cmdline.at(i+1); i++; continue;
		}
		if (tmp == "-s") {
			db_path = cmdline.at(i+1); i++; continue;
		}
		if (tmp == "-p") {
			port = cmdline.at(i+1).toInt(); i++; continue;
		}
		if (tmp == "-t") {
			init_seed = cmdline.at(i+1); i++; continue;
		}
		// ignore unrecognized args
	}
}

