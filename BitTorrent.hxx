#pragma once

#include <QCoreApplication>
#include <QDebug>
#include <QFile>

#include <memory>

class BtString;
class BtInteger;
class BtList;
class BtDictionary;

class BtNode
{
public:
	virtual class BtString * asString(void) { return 0; }
	virtual class BtInteger * asInteger(void) { return 0; }
	virtual class BtList * asList(void) { return 0; }
	virtual class BtDictionary * asDictionary(void) { return 0; }
	virtual const QString print(void) = 0;
};

class BtString : public BtNode
{
private:
	QByteArray data;
public:
	class BtString * asString(void) override { return this; }
	const QString value(void) const
	{
		QString s = QString::fromUtf8(data);
		// I have no idea why I wrote this...
		if (s.toUtf8().length() == data.length())
			return s;
		return data.toHex();
	}
	BtString(const QByteArray & data) : data(data) {}
	const QString print(void) override { return QString("\"%1\"").arg(value()); }
};
class BtInteger : public BtNode
{
private:
	int64_t i;
public:
	class BtInteger * asInteger(void) override { return this; }
	int64_t value(void) { return i; }
	BtInteger(int64_t i) : i(i) {}
	const QString print(void) override { return QString("%1").arg(i); }
};
class BtList : public BtNode
{
private:
	const QList<std::shared_ptr<BtNode>> l;
public:
	class BtList * asList(void) override { return this; }
	const QList<std::shared_ptr<BtNode>> & value(void) const { return l; }
	BtList(const QList<std::shared_ptr<BtNode>> & l) : l(l) {}
	const QString print(void) override
	{
		QString s("[");
		for (const auto & item : l)
			s += item->print() + ", ";
		if (s.length() > 1 && s.last(2) == ", ")
			s.chop(2);
		return s + ']';
	}
};
class BtDictionary : public BtNode
{
private:
	const QList<QPair<QString, std::shared_ptr<BtNode>>> d;
public:
	class BtDictionary * asDictionary(void) override { return this; }
	const QList<QPair<QString, std::shared_ptr<BtNode>>> & value(void) const { return d; }
	BtDictionary(const QList<QPair<QString, std::shared_ptr<BtNode>>> & d) : d(d) {}
	const QString print(void) override
	{
		QString s("{");
		for (const auto & item : d)
			s += QString("\"%1\" : %2, ").arg(item.first).arg(item.second->print());
		if (s.length() > 1 && s.last(2) == ", ")
			s.chop(2);
		return s + '}';
	}
};

std::shared_ptr<BtNode> parseString(const QByteArray & data, int & offset)
{
	int x = offset;
	if (x >= data.length())
		return 0;
	int len = 0;
	while (x < data.length() && QChar(data.at(x)).isDigit())
		len = len * 10 + QChar(data.at(x ++)).digitValue();
	/* Is a zero-length string allowed? */
	if (!len)
		return 0;
	if  (x >= data.length() || data.at(x) != ':')
	{
		qCritical() << __func__ << "error at offsets" << offset << x;
		return 0;
	}
	QByteArray s = data.mid(x + 1, len);
	if (s.length() != len)
	{
		qCritical() << __func__ << "error at offsets" << offset << x;
		return 0;
	}
	offset = x + 1 + len;
	return std::make_shared<BtString>(s);
}

std::shared_ptr<BtNode> parseInteger(const QByteArray & data, int & offset)
{
	int x = offset;
	if (x >= data.length() || data.at(x) != 'i')
		return 0;
	x ++;
	int64_t i = 0;
	int sgn = 1;
	while (x < data.length())
	{
		QChar c = data.at(x ++);
		if (c.isDigit())
			i = i * 10 + c.digitValue();
		else if (c == '-')
			sgn = -1;
		else if (c == 'e')
			break;
		else
		{
			qCritical() << __func__ << "error at offsets" << offset << x;
			return 0;
		}
	}
	offset = x;
	return std::make_shared<BtInteger>(i * sgn);
}

std::shared_ptr<BtNode> parseDictionary(const QByteArray & data, int & offset);

std::shared_ptr<BtNode> parseList(const QByteArray & data, int & offset)
{
	int x = offset;
	if (x >= data.length() || data.at(x) != 'l')
		return 0;
	x ++;
	QList<std::shared_ptr<BtNode>> l;
	while (1)
	{
		std::shared_ptr<BtNode> t;
		if ((t = parseString(data, x)))
			l.push_back(t);
		else if ((t = parseInteger(data, x)))
			l.push_back(t);
		else if ((t = parseList(data, x)))
			l.push_back(t);
		else if ((t = parseDictionary(data, x)))
			l.push_back(t);
		else if (x < data.length() && data.at(x) == 'e')
		{
			offset = x + 1;
			return std::make_shared<BtList>(l);
		}
		else
		{
			qCritical() << __func__ << "error at offsets" << offset << x;
			return 0;
		}
	}
}

std::shared_ptr<BtNode> parseDictionary(const QByteArray & data, int & offset)
{
	int x = offset;
	if (x >= data.length() || data.at(x) != 'd')
		return 0;
	x ++;
	QList<QPair<QString, std::shared_ptr<BtNode>>> d;
	while (1)
	{
		QString key;
		std::shared_ptr<BtNode> value;

		if (x < data.length() && data.at(x) == 'e')
		{
			offset = x + 1;
			return std::make_shared<BtDictionary>(d);
		}

		if (!(value = parseString(data, x)))
			return 0;
		key = value->asString()->value();

		if ((value = parseString(data, x)))
			d.push_back(QPair<QString, std::shared_ptr<BtNode>>(key, value));
		else if ((value = parseInteger(data, x)))
			d.push_back(QPair<QString, std::shared_ptr<BtNode>>(key, value));
		else if ((value = parseList(data, x)))
			d.push_back(QPair<QString, std::shared_ptr<BtNode>>(key, value));
		else if ((value = parseDictionary(data, x)))
			d.push_back(QPair<QString, std::shared_ptr<BtNode>>(key, value));
		else
		{
			qCritical() << __func__ << "error at offset" << offset << x;
			return 0;
		}
	}
}


class BitTorrent
{
	Q_DECLARE_TR_FUNCTIONS(BitTorrent)
private:

	bool process_files_dictionary_entry(const std::shared_ptr<BtNode> entry)
	{
		if (!entry->asDictionary())
			return false;
		QStringList path;
		int64_t length = -1;
		for (const auto & item : entry->asDictionary()->value())
		{
			if (item.first == "path" && item.second->asList())
			{
				for (const auto & path_item : item.second->asList()->value())
				{
					if (!path_item->asString())
						return false;
					path << path_item->asString()->value();
				}
			}
			else if (item.first == "length" && item.second->asInteger())
				length = item.second->asInteger()->value();
		}
		if (!path.size() || length == -1)
			return false;
		torrent_details.files << TorrentDetails::file_info(path, length);
		return true;
	}
	bool process_file_info_dictionary(const std::shared_ptr<BtNode> root)
	{
		BtDictionary * d;
		if (!(d = root->asDictionary()))
		{
			qCritical() << "Could not process the torrent root node as a dictionary.";
			return false;
		}
		std::shared_ptr<BtNode> info_dictionary;
		for (const auto & item : d->value())
		{
			if (item.first == "info")
				info_dictionary = item.second;
		}
		if (!info_dictionary.get() || !info_dictionary->asDictionary())
		{
			qCritical() << "Could not find the 'info' dictionary entry in torrent.";
			return false;
		}
		for (const auto & item : info_dictionary->asDictionary()->value())
		{
			if (item.first == "files")
			{
				BtList * l = item.second->asList();
				if (!l)
				{
					qCritical() << "Could not process the 'files' entry as a list.";
					return false;
				}
				for (const auto & item : l->value())
					if (!process_files_dictionary_entry(item))
					{
						qCritical() << "Could not process a 'files' entry dictionary.";
						return false;
					}
			}
			else if (item.first == "length" && item.second->asInteger())
				torrent_details.length = item.second->asInteger()->value();
			else if (item.first == "name" && item.second->asString())
				torrent_details.name = item.second->asString()->value();
			else if (item.first == "piece length" && item.second->asInteger())
				torrent_details.piece_length = item.second->asInteger()->value();
			else if (item.first == "pieces" && item.second->asString())
			{
				torrent_details.piece_sha1_hashes = item.second->asString()->value();
				if (torrent_details.piece_sha1_hashes.length() % SHA1_CHECKSUM_BYTESIZE)
				{
					qCritical() << "Bad torrent hashes string, not a multiple of" << SHA1_CHECKSUM_BYTESIZE << ".";
					return false;
				}

			}
			else
			{
				/*! \todo	Resolve this - what is the 'name.utf-8' key supposed to mean?!?!
				 * 		Ignore for the time being, time is getting short... */
				if (item.first == "name.utf-8")
					qCritical() << "!!! HANDLE THE 'name.utf-8' KEY (WHAT IS THIS???) !!!\nIgnore this now, time is getting short...";
				else if (item.first == "md5sum")
					qCritical() << "Handle the" << item.first << "key, ignoring now.";
				else
				{
					qCritical() << "Unrecognized key in the torrent 'info' dictionary:" << item.first;
					return false;
				}
			}
		}

		if (		0
				|| !torrent_details.name.length()
				|| torrent_details.piece_length == -1
				|| !torrent_details.piece_sha1_hashes.length()
				|| (
					(torrent_details.length == -1 && !torrent_details.files.length())
					|| (torrent_details.length != -1 && torrent_details.files.length())
					)
				)
		{
			qCritical() << "Could not process torrent file:" << torrent_file_name;
			return false;
		}
		return true;
	}
#if 0
	void extract_file_data(const std::shared_ptr<BtNode> node)
	{
		BtDictionary * d;
		if (!(d = node->asDictionary()))
			return;
		for (const auto & item : d->value())
		{
			if (item.first == "info")
				extract_file_data(item.second);
			else if (item.first == "files")
			{
				BtList * l = item.second->asList();
				if (!l)
					continue;
				for (const auto & item : l->value())
					extract_file_data(item);
			}
			else if (item.first == "length" && item.second->asInteger())
				torrent_details.length = item.second->asInteger()->value();
			else if (item.first == "name" && item.second->asString())
				torrent_details.name = item.second->asString()->value();
			else if (item.first == "piece length" && item.second->asInteger())
				torrent_details.piece_length = item.second->asInteger()->value();
			else if (item.first == "pieces" && item.second->asString())
				torrent_details.piece_hashes = item.second->asString()->value();
		}
	}
#endif
	const QString torrent_file_name;
public:

	enum
	{
		SHA1_CHECKSUM_BYTESIZE	= 40 /* 160 bits ---> 20 bytes * 2 hex characters = 40 bytes */
	};

	struct TorrentDetails
	{
		struct file_info { QStringList path; int64_t length; file_info(const QStringList & path, int64_t length) : path(path), length(length) {}};
		QList<struct file_info> files;
		/* Note: there are two cases, depending on if the torrent contains a single file, or a list of files.
		 *
		 * In case of a single file in the torrent:
		 * - the 'length' field is present and contains the file size
		 * - the 'files' dictionary is missing; therefore the 'files' list will be empty
		 * - the 'name' field holds the name of the file
		 *
		 * In case of multiple files in the torrent:
		 * - the 'length' field is absemt
		 * - the 'files' dictionary is present, the 'files' list will hold the list of files
		 * - the 'name' field holds the name of the directory that files should be stored into
		 */
		QString name;
		int64_t piece_length = -1;
		int64_t length = -1;
		QString piece_sha1_hashes;
	}
	torrent_details;


public:
	BitTorrent(const QString & torrent_file_name) : torrent_file_name(torrent_file_name) {}

	bool parse(void)
	{
		QFile f(torrent_file_name);
		if (!f.open(QFile::ReadOnly))
		{
			qCritical() << tr("Failed to open torrent file for reading:") << torrent_file_name;
			return false;
		}
		const QByteArray data = f.readAll();
		int offset = 0;
		const std::shared_ptr<BtNode> root = parseDictionary(data, offset);


		if (root.get())
		{
//#ifdef BITTORRENT_DEBUG
			QFile f("torrent-dump.txt");
			f.open(QFile::WriteOnly);
			QString s = root->print();
			f.write(s.toLocal8Bit());
			//qInfo() << "Torrent data:" << s;
//#endif
		}
		else
			return false;

		//extract_file_data(root);
		if (!process_file_info_dictionary(root))
			return false;
		return true;
	}

};
