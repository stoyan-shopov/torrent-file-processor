#include <QCoreApplication>
#include <QTranslator>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QCommandLineParser>

#include <functional>

#include "BitTorrent.hxx"


static bool verify_torrent_hashes(const QString & directoryName, const BitTorrent & bitTorrent)
{
	/* The list of files in the torrent piece currently verified - not including the currently processed file. */
	QElapsedTimer timer;
	timer.start();
	QStringList current_piece_files_stack;
	const uint64_t piece_length = bitTorrent.torrent_details.piece_length;
	uint64_t total_length = 0;
	QString h = bitTorrent.torrent_details.piece_sha1_hashes;
	assert(h.length() % BitTorrent::SHA1_CHECKSUM_BYTESIZE == 0);
	QStringList hashes;
	do
	{
		hashes << h.left(BitTorrent::SHA1_CHECKSUM_BYTESIZE);
		h.remove(0, BitTorrent::SHA1_CHECKSUM_BYTESIZE);
	}
	while (h.length());

	/* Construct the list of filenames. */
	QStringList fileNames;
	QList<uint64_t> fileSizes;
	if (!bitTorrent.torrent_details.files.length())
	{
		/* Single-file torrent. */
		fileNames << directoryName + '/' + bitTorrent.torrent_details.name;
		fileSizes << bitTorrent.torrent_details.length;
	}
	else
	{
		/* Multiple files in torrent. */
		for (const auto & f : bitTorrent.torrent_details.files)
		{
			QString t(directoryName + '/' + bitTorrent.torrent_details.name);
			for (const auto & f : f.path)
				t += '/' + f;
			fileNames << t;
			fileSizes << f.length;
		}
	}
	QByteArray data;

	std::function<bool(const QString & current_file)> verifyHash = [&] (const QString & current_file) -> bool {
		//qDebug() << "Hash debug:" << QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex() << hashes.front();
		if (QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex().toLower() != hashes.front().toLower())
		{
			QString affectedFiles;
			for (const auto & t : current_piece_files_stack)
				affectedFiles += '"' + t + '"' + ", ";
			affectedFiles += '"' + current_file + '"';
			qCritical().noquote() << QCoreApplication::translate("Main", "SHA1 hash mismatch, affected file(s) in the corrupted torrent piece:") << affectedFiles;
			qCritical() << "More files possibly affected, aborting torrent checksum verification.";
			return false;
		}
		current_piece_files_stack.clear();
		hashes.pop_front();
		total_length += data.length();
		data.clear();
		return true;
	};

	for (const auto & f : fileNames)
	{
		assert(data.length() < piece_length);
		QFileInfo fi(f);
		if (!fi.exists())
		{
			qCritical() << "File does not exist:" << f;
			return false;
		}
		if (!fi.isFile())
		{
			qCritical() << "Invalid filename, not a file:" << f;
			return false;
		}
		if (fi.size() != fileSizes.front())
		{
			qCritical() << "File size mismatch for file" << f << "Expected:" << fileSizes.front() << ", actual:" << fi.size();
			return false;
		}

		QFile file(f);
		if (!file.open(QFile::ReadOnly))
		{
			qCritical() << "Could not open file for reading:" << f;
			return false;
		}
		while (!file.atEnd())
		{
			data += file.read(piece_length - data.length());
			if (data.length() == piece_length)
			{
				if (!verifyHash(f))
					return false;
			}
		}
		if (data.length())
			current_piece_files_stack << f;
		qDebug() << "Processed file" << f;

		fileSizes.pop_front();
	}
	/* Handle last data piece. */
	if (data.length() && !verifyHash(fileNames.last()))
		return false;

	uint64_t milliseconds = timer.elapsed();
	qInfo().noquote() << QString("Average speed %2 megabytes/second.")
			     .arg((((double) total_length / milliseconds) * 1000.) / (1 << 20));

	return true;
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	QTranslator translator;
	if (!translator.load(":/torrent-file-processor_bg.qm"))
		qDebug() << "Error loading translation.";
	application.installTranslator(& translator);
	QDir torrent_directory("x:/Annas Archive torrents");
	QStringList torrent_files =  torrent_directory.entryList();
	int64_t total_length = 0, total_file_count = 0, total_torrents_processed = 0;

	if (1)
	{
		torrent_files.clear();
		torrent_directory.setPath("");
		torrent_files << "X:/Торенти на български книги - да се коригират имената и да се сложат в архива/Ethnology.torrent";
	}

	for (const auto & torrent_file : torrent_files)
	{
		qInfo() << "Processing torrent file:" << torrent_file;
		QString torrent_file_name;
		if (!torrent_directory.path().isEmpty())
			torrent_file_name += torrent_directory.path() + '/';
		torrent_file_name += torrent_file;
		BitTorrent t(torrent_file_name);
		if (!t.parse())
		{
			qCritical().noquote() << "Failed to process file" << torrent_file << "as a torrent file.";
			continue;
		}
		if (!t.torrent_details.files.length())
		{
			qInfo() << "Torrent filename:" << t.torrent_details.name << ". Size:" << t.torrent_details.length;
			total_length += t.torrent_details.length;
			total_file_count ++;
		}
		else
		{
			int64_t l = 0;
			qInfo() << "Torrent directory:" << t.torrent_details.name;
			qInfo() << "Piece length:" << t.torrent_details.piece_length;
			qInfo() << "Files in torrent:" << t.torrent_details.files.length();
			for (const auto & file : t.torrent_details.files)
				l += file.length;
			qInfo() << "Total data size:" << l << "bytes," << (double) l / (1024 * 1024 * 1024) << "gigabytes";
			total_length += l;
			total_file_count += t.torrent_details.files.count();
			verify_torrent_hashes("x:/Торенти на български книги - да се коригират имената и да се сложат в архива", t);
		}
		total_torrents_processed ++;
	}
	qInfo() << "Total torrents processed:" << total_torrents_processed;
	qInfo() << "Total file count:" << total_file_count;
	qInfo() << "Total data size:" << total_length << "bytes," << (double) total_length / (1024 * 1024 * 1024) << "gigabytes," << ((double) total_length / (1024 * 1024 * 1024)) / 1024 << "terabytes";
	return 0;
}
