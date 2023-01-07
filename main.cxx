#include <QCoreApplication>
#include <QTranslator>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QCommandLineParser>

#include <functional>

#include "BitTorrent.hxx"


static bool verify_torrent_hashes(const QString & torrentDataDirectoryName, const BitTorrent & bitTorrent,
		bool verboseFlag, bool checkSizeOnlyFlag)
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
		fileNames << torrentDataDirectoryName + '/' + bitTorrent.torrent_details.name;
		fileSizes << bitTorrent.torrent_details.length;
	}
	else
	{
		/* Multiple files in torrent. */
		for (const auto & f : bitTorrent.torrent_details.files)
		{
			QString t(torrentDataDirectoryName + '/' + bitTorrent.torrent_details.name);
			for (const auto & f : f.path)
				t += '/' + f;
			fileNames << t;
			fileSizes << f.length;
		}
	}
	QByteArray data;

	std::function<bool(const QString & current_file)> verifyHash = [&] (const QString & current_file) -> bool {
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

		if (!checkSizeOnlyFlag)
		{
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
			if (verboseFlag)
				qInfo() << "Processed file" << f;
		}

		fileSizes.pop_front();
	}
	/* Handle last data piece. */
	if (!checkSizeOnlyFlag && data.length() && !verifyHash(fileNames.last()))
		return false;

	uint64_t milliseconds = timer.elapsed();
	if (!checkSizeOnlyFlag)
		qInfo().noquote() << QString("Average speed %2 megabytes/second.")
				     .arg((((double) total_length / milliseconds) * 1000.) / (1 << 20));

	return true;
}

int main(int argc, char *argv[])
//int wmain(int argc, wchar_t *argv[])
{
	QCoreApplication application(argc, argv);

	QTranslator translator;
	if (!translator.load(":/torrent-file-processor_bg.qm"))
		qCritical() << "Error loading translation.";
	/*! \todo	For some reason, setting the translator in windows 10 causes garbage to be printed in the console. */
	if (0)
	application.installTranslator(& translator);
	int64_t total_length = 0, total_file_count = 0, total_torrents_processed = 0;

	std::function<void(void)> printUsage = [] (void) -> void {
		qInfo() << "Torrent data verifier.";
		qInfo() << "Verifies downloaded torrent files by computing the torrent SHA1 checksums.";
		qInfo() << "";
		qInfo() << "Usage:";
		qInfo() << "libgen-torrent-data-verifier [-h] [-v] [-c] [-l] torrent-data-directory torrent-source";
		qInfo() << "";
		qInfo() << "Options:";
		qInfo() << "-h | --help	Print this usage information.";
		qInfo() << "-v | --verbose	Turn on verbose reporting.";
		qInfo() << "-c | --continue	Do not stop on errors, process all torrents specified.";
		qInfo() << "-l | --torrent-list		The specified 'torrent-source' argument is a text file containing a list of torrent file names (separated by newlines) to be verified.";
		qInfo() << "			If this flag is not specified, the 'torrent-source' argument is the name of a single torrent file to be verified.";
		qInfo() << "-z | --check-size-only	Only check file sizes, and do not compute torrent checksums.";
		qInfo() << "";
		qInfo() << "A torrent data directory MUST always be specified.";
		qInfo() << "Specify EITHER a text file containing the torrent files to be verified (with the '-l' switch), OR a single torrent file name.";
		qInfo() << "If a text file containing the list of torrents to be verified is specified,";
		qInfo() << "empty lines and lines starting with a number-sign ('#') are allowed and ignored.";
		qInfo() << "";
		qInfo() << "Examples:";
		qInfo() << "";
		qInfo() << "Verify a single torrent:";
		qInfo() << "libgen-torrent-data-verifier /torrents-data-directory/ r_1142000.torrent";
		qInfo() << "";
		qInfo() << "Verify the list of torrents contained in a text file:";
		qInfo() << "libgen-torrent-data-verifier -l /torrents-data-directory/ torrent-list.txt";
	};

	application.setApplicationName("torrent-data-verifier");

	QCommandLineParser cp;
	cp.setApplicationDescription("Torrent data verifier");
	cp.addPositionalArgument("torrent-data-directory", "The data directory that contains downloaded torrents.");
	cp.addPositionalArgument("torrent-source", "The torrent file name, or the file containing a list of torrent file names to verify.");

	QCommandLineOption helpOption(QStringList() << "h" << "help", "Print usage information.");
	cp.addOption(helpOption);

	QCommandLineOption verboseOption(QStringList() << "v" << "verbose", "Turn on verbose reporting.");
	cp.addOption(verboseOption);

	QCommandLineOption continueOption(QStringList() << "c" << "continue", "Do not stop on errors, process all torrents specified.");
	cp.addOption(continueOption);

	QCommandLineOption torrentListOption(QStringList() << "l" << "torrent-list", "The 'torrent-source' argument is a text file containing the list of torrents to be verified.");
	cp.addOption(torrentListOption);

	QCommandLineOption sizeOnlyOption(QStringList() << "z" << "check-size-only", "Only check file sizes, and do not compute torrent checksums.");
	cp.addOption(sizeOnlyOption);

	cp.process(application);
	if (cp.isSet(helpOption))
	{
		printUsage();
		return 0;
	}

	const bool verboseFlag = cp.isSet(verboseOption);
	const bool continueOnErrorsFlag = cp.isSet(continueOption);
	const bool torrentListFlag = cp.isSet(torrentListOption);
	const bool checkSizeOnlyFlag = cp.isSet(sizeOnlyOption);

	if (cp.positionalArguments().length() != 2)
	{
		qCritical() << "Invalid arguments, need to specify both a torrent directory, and a torrent source (either a torrent file name, or a file containing a list of torrents).";
		qCritical() << "";
		printUsage();
		return 1;
	}

	const QString torrent_data_directory = cp.positionalArguments().at(0);
	const QString torrent_source = cp.positionalArguments().at(1);

	/* Build the list of torrents to be verified. */
	QStringList torrent_files;
	if (torrentListFlag)
	{
		/* The list of torrent files is specified in a text file. */
		QFile f(torrent_source);
		if (!f.open(QFile::ReadOnly))
		{
			qCritical() << "Can not open torrent list file for reading:" << torrent_source;
			return 1;
		}
		QStringList lines = QString(f.readAll()).split('\n');
		for (const auto & line : lines)
		{
			QString l = line.trimmed();
			if (!l.startsWith('#') && !l.isEmpty())
				torrent_files << l;
		}
	}
	else
		/* Verify a single torrent specified on the command line. */
		torrent_files << torrent_source;

	for (const auto & torrent_file : torrent_files)
	{
		qInfo() << "Processing torrent file:" << torrent_file;
		BitTorrent t(torrent_file);
		if (!t.parse())
		{
			qCritical().noquote() << "Failed to process file" << torrent_file << "as a torrent file.";
			continue;
		}
		if (!t.torrent_details.files.length())
		{
			if (verboseFlag)
				qInfo() << "Torrent filename:" << t.torrent_details.name << ". Size:" << t.torrent_details.length;
			total_length += t.torrent_details.length;
			total_file_count ++;
		}
		else
		{
			int64_t l = 0;
			if (verboseFlag)
			{
				qInfo() << "Torrent directory:" << t.torrent_details.name;
				qInfo() << "Piece length:" << t.torrent_details.piece_length;
				qInfo() << "Files in torrent:" << t.torrent_details.files.length();
			}
			for (const auto & file : t.torrent_details.files)
				l += file.length;
			if (verboseFlag)
				qInfo() << "Total data size:" << l << "bytes," << (double) l / (1024 * 1024 * 1024) << "gigabytes";
			total_length += l;
			total_file_count += t.torrent_details.files.count();
		}
		total_torrents_processed ++;
		if (!verify_torrent_hashes(torrent_data_directory, t, verboseFlag, checkSizeOnlyFlag))
		{
			qCritical() << "Error processing torrent:" << torrent_file;
			if (!continueOnErrorsFlag)
			{
				qCritical() << "Aborting torrent processing.";
				break;
			}
		}
	}
	qInfo() << "Total torrents processed:" << total_torrents_processed;
	qInfo() << "Total file count:" << total_file_count;
	qInfo() << "Total data size:" << total_length << "bytes," << (double) total_length / (1024 * 1024 * 1024) << "gigabytes," << ((double) total_length / (1024 * 1024 * 1024)) / 1024 << "terabytes";
	return 0;
}
