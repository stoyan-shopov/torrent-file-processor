#include <QCoreApplication>
#include <QTranslator>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QCommandLineParser>
#include <QDateTime>

#include <functional>

#include "BitTorrent.hxx"


enum
{
	MD5_HASH_STRING_LENGTH	= 32,
};

struct TorrentCheckResult
{
	QString		torrent_filename;
	/* If any corrupted pieces are found in the torrent, when SHA1 checksums of data pieces in the torrent is performed,
	 * this list will hold all affected files. */
	QStringList	corrupted_files_by_sha1_checksum;
	/* If MD5 checksumming of data for files which names *look* like an MD5 hash value is performed,
	 * this list will hold any files that have been found to be corrupted when checking the MD5 hashes. */
	QStringList	corrupted_files_by_md5_checksum;

	TorrentCheckResult(const QString & torrent_filename) : torrent_filename(torrent_filename){}
};

static bool verify_torrent_hashes(const QString & torrentDataDirectoryName, const BitTorrent & bitTorrent,
		bool verboseFlag, bool checkSizeOnlyFlag, bool computeMd5Hashes, struct TorrentCheckResult & checkResult)
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
		bool result = true;
		if (QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex().toLower() != hashes.front().toLower())
		{
			QString affectedFiles;
			for (const auto & t : current_piece_files_stack)
				affectedFiles += '"' + t + '"' + ", ";
			affectedFiles += '"' + current_file + '"';
			qCritical().noquote() << QCoreApplication::translate("Main", "ERROR: SHA1 hash mismatch, affected file(s) in the corrupted torrent piece:") << affectedFiles;
			checkResult.corrupted_files_by_sha1_checksum << current_piece_files_stack;
			result = false;
		}
		current_piece_files_stack.clear();
		hashes.pop_front();
		total_length += data.length();
		data.clear();
		return result;
	};

	bool result = true;
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

			/* If a computation of md5 hash checksums is requested, and if the filename *looks* like an md5 hash value,
			 * compute the md5 hash and compare it to the filename. */
			/* Remove any file extensions. */
			QString md5HashFromFilename = QFileInfo(f).fileName().split('.').at(0);
			md5HashFromFilename = md5HashFromFilename.split('.').at(0);
			bool md5Flag = (md5HashFromFilename.length() == MD5_HASH_STRING_LENGTH);
			/* See if the filename looks like an md5 hash value. */
			if (md5Flag)
				for (const auto & c : md5HashFromFilename)
					if (		   !('0' <= c && c <= '9')
							&& !('a' <= c && c <= 'f')
							&& !('A' <= c && c <= 'F')
							)
					{
						md5Flag = false;
						break;
					}
			md5Flag = computeMd5Hashes && md5Flag;
			QCryptographicHash md5hash(QCryptographicHash::Md5);

			while (!file.atEnd())
			{
				const QByteArray dataPiece = file.read(piece_length - data.length());
				data += dataPiece;
				if (data.length() == piece_length)
					result &= verifyHash(f);
				if (md5Flag)
					md5hash.addData(dataPiece);
			}
			if (data.length())
				current_piece_files_stack << f;
			if (verboseFlag)
			{
				qInfo() << "Processed file" << f;
				if (computeMd5Hashes)
				{
					if (md5Flag)
						qInfo() << "Also computed the MD5 hash for file" << f;
					else
						qInfo() << "NOTE: requested computing the MD5 hash for file" << f << ", but filename not recognized as an MD5 hash value, did not compute MD5 hash value for this file.";
				}
			}
			if (md5Flag && md5hash.result().toHex().toLower() != md5HashFromFilename)
			{
				result = false;
				qCritical().noquote() << "ERROR: MD5 hash mismatch for file" << f;
				qCritical().noquote() << "ERROR: expected MD5 hash:" << md5HashFromFilename.toLower() << "; computed MD5 hash:" << md5hash.result().toHex().toLower();
				checkResult.corrupted_files_by_md5_checksum << f;
			}

		}

		fileSizes.pop_front();
	}
	/* Handle last data piece. */
	if (!checkSizeOnlyFlag && data.length() && !verifyHash(fileNames.last()))
		result = false;

	uint64_t milliseconds = timer.elapsed();
	if (!checkSizeOnlyFlag)
		qInfo().noquote() << QString("Average speed %2 megabytes/second.")
				     .arg((((double) total_length / milliseconds) * 1000.) / (1 << 20));

	return result;
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
	uint64_t total_length = 0, total_file_count = 0, total_torrents_processed = 0;

	std::function<void(void)> printUsage = [] (void) -> void {
		qInfo() << "Torrent data verifier.";
		qInfo() << "Verifies downloaded torrent files by computing the torrent SHA1 checksums.";
		qInfo() << "";
		qInfo() << "Usage:";
		qInfo() << "libgen-torrent-data-verifier [-h] [-v] [-c] [-l] [-z] torrent-data-directory torrent-source";
		qInfo() << "";
		qInfo() << "Options:";
		qInfo() << "-h | --help	Print this usage information.";
		qInfo() << "-d | --dump	Only dump torrent file details, do not perform torrent data verification.";
		qInfo() << "-v | --verbose	Turn on verbose reporting.";
		qInfo() << "-c | --continue	Do not stop on errors, process all torrents specified.";
		qInfo() << "-l | --torrent-list		The specified 'torrent-source' argument is a text file containing a list of torrent file names (separated by newlines) to be verified.";
		qInfo() << "			If this flag is not specified, the 'torrent-source' argument is the name of a single torrent file to be verified.";
		qInfo() << "-m | --md5		Also compute and check MD5 hash checksums for files with names which *look* like and MD5 hash value.";
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

	QCommandLineOption dumpOption(QStringList() << "d" << "dump", "Only dump torrent information, do not verify data");
	cp.addOption(dumpOption);

	QCommandLineOption verboseOption(QStringList() << "v" << "verbose", "Turn on verbose reporting.");
	cp.addOption(verboseOption);

	QCommandLineOption continueOption(QStringList() << "c" << "continue", "Do not stop on errors, process all torrents specified.");
	cp.addOption(continueOption);

	QCommandLineOption torrentListOption(QStringList() << "l" << "torrent-list", "The 'torrent-source' argument is a text file containing the list of torrents to be verified.");
	cp.addOption(torrentListOption);

	QCommandLineOption md5Option(QStringList() << "m" << "md5", "Also compute and check MD5 hash checksums for files with names which *look* like and MD5 hash value.");
	cp.addOption(md5Option);

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
	const bool dumpOnlyFlag = cp.isSet(dumpOption);
	const bool md5Flag = cp.isSet(md5Option);

	/* Validate arguments. */
	if (!dumpOnlyFlag && cp.positionalArguments().length() != 2)
	{
		qCritical() << "Invalid arguments, need to specify both a torrent directory, and a torrent source (either a torrent file name, or a file containing a list of torrents).";
		qCritical() << "";
		printUsage();
		return 1;
	}
	else if (dumpOnlyFlag && cp.positionalArguments().length() != 1)
	{
		qCritical() << "Invalid arguments, need to specify a torrent source (either a torrent file name, or a file containing a list of torrents).";
		qCritical() << "";
		printUsage();
		return 1;
	}

	const QString torrent_data_directory = (dumpOnlyFlag ? "" : cp.positionalArguments().at(0));
	const QString torrent_source = cp.positionalArguments().at(dumpOnlyFlag ? 0 : 1);

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

	QList<TorrentCheckResult> checkResults;

	struct
	{
		unsigned file_count = 0;
		uint64_t total_data_length = 0;
	}
	torrent_statistics;

	/* Compute total number of files and total data length of the files in all torrents,
	 * in order to be able to print percentage statistics during processing. */
	for (const auto & torrent_file : torrent_files)
	{
		BitTorrent t(torrent_file);
		if (!t.parse())
		{
			qCritical().noquote() << "Failed to process file" << torrent_file << "as a torrent file.";
			return -1;
		}
		if (!t.torrent_details.files.length())
		{
			torrent_statistics.total_data_length += t.torrent_details.length;
			torrent_statistics.file_count ++;
		}
		else
		{
			for (const auto & file : t.torrent_details.files)
				torrent_statistics.total_data_length += file.length;
			torrent_statistics.file_count += t.torrent_details.files.count();
		}
	}

	QElapsedTimer timer;
	timer.start();
	const QByteArray logFileLineDelimiter = "-----------------------------------------------\n";

	QFile logFile(QString("torrent-check-log-%1.txt").arg(QDateTime::currentDateTime().toString("ddMMyyyy-hhmmss")));
	if (!logFile.open(QFile::WriteOnly))
	{
		qCritical().noquote() << "Can not open log file for writing:" << logFile.fileName();
		return 1;
	}

	logFile.write(QString("Torrent data verification started, current time: %1\n\n").arg(QDateTime::currentDateTime().toString("dd/MM/yyyy, hh:mm:ss")).toLocal8Bit());
	{
		QString cmdline;
		int i;
		for (i = 0; i < argc; i ++)
			cmdline += QString(argv[i]) + ' ';
		logFile.write(QString("Command line:\n%1\n").arg(cmdline).toLocal8Bit());
	}
	logFile.write(logFileLineDelimiter);
	logFile.write("Verifying torrents:\n");
	logFile.write(logFileLineDelimiter);
	logFile.flush();

	for (const auto & torrent_file : torrent_files)
	{
		qInfo() << "----------------------------------------------------";
		qInfo().noquote() << "Processing torrent file:" << torrent_file
			<< QString(": %1 files out of %2 (%3 %),")
			   .arg(total_file_count).arg(torrent_statistics.file_count).arg(((double) total_file_count * 100.) / torrent_statistics.file_count, 0, 'f', 2)
			<< QString("%1 bytes out of %2 (%3 %) processed,")
			   .arg(total_length).arg(torrent_statistics.total_data_length).arg(((double) total_length * 100.) / torrent_statistics.total_data_length, 0, 'f', 2)
			<< QString("%1 seconds (%2 hours) elapsed")
			   .arg(timer.elapsed() / 1000).arg((double) timer.elapsed() / (3600 * 1000), 0, 'f', 2);
		BitTorrent t(torrent_file);
		if (!t.parse())
		{
			qCritical().noquote() << "Failed to process file" << torrent_file << "as a torrent file.";
			return -1;
		}
		qInfo() << "Processing torrent:" << torrent_file;
		if (dumpOnlyFlag)
			logFile.write(QString("Processing torrent: %1\n").arg(torrent_file).toLocal8Bit());
		if (!t.torrent_details.files.length())
		{
			if (dumpOnlyFlag)
			{
				QString s = QString("Single-file torrent, name: %1, size: %2").arg(t.torrent_details.name).arg(t.torrent_details.length);
				qInfo().noquote() << s;
				logFile.write((s + '\n').toLocal8Bit());
			}
			else if (verboseFlag)
				qInfo() << "Single-file torrent, name:" << t.torrent_details.name << ". Size:" << t.torrent_details.length;
			total_length += t.torrent_details.length;
			total_file_count ++;
		}
		else
		{
			uint64_t l = 0;
			if (dumpOnlyFlag)
			{
				qInfo() << "Torrent directory:" << t.torrent_details.name;
				logFile.write(QString("Torrent directory: %1\n").arg(t.torrent_details.name).toLocal8Bit());
				qInfo() << "Files in torrent:";
				logFile.write("Files in torrent:\n\n");
				for (const auto & f : t.torrent_details.files)
				{
					QString s;
					for (const auto & p : f.path)
						s += p + '/';
					/* Remove last slash character. */
					s.chop(1);
					qInfo().noquote() << s;
					logFile.write((s + '\n').toLocal8Bit());
				}
			}
			if (verboseFlag)
			{
				qInfo() << "Torrent directory:" << t.torrent_details.name;
				qInfo() << "Piece length:" << t.torrent_details.piece_length;
				qInfo() << "Number of files in torrent:" << t.torrent_details.files.length();
			}
			for (const auto & file : t.torrent_details.files)
				l += file.length;
			if (verboseFlag)
				qInfo() << "Total data size:" << l << "bytes," << (double) l / (1024 * 1024 * 1024) << "gigabytes";
			total_length += l;
			total_file_count += t.torrent_details.files.count();
		}
		total_torrents_processed ++;
		if (!dumpOnlyFlag)
		{
			TorrentCheckResult checkResult(torrent_file);

			if (!verify_torrent_hashes(torrent_data_directory, t, verboseFlag, checkSizeOnlyFlag, md5Flag, checkResult))
			{
				checkResults << checkResult;
				qCritical() << "Error processing torrent:" << torrent_file;
				logFile.write(QString("%1\t: ERROR!!!\n").arg(torrent_file).toLocal8Bit());
				if (checkResult.corrupted_files_by_sha1_checksum.length())
				{
					qCritical() << "Corrupted files in torrent:" << checkResult.corrupted_files_by_sha1_checksum.join(", ");
					logFile.write(QString("%1\t: ERROR, corrupted files in torrent, SHA1 hash mismatch: %2\n")
						      .arg(torrent_file)
						      .arg(checkResult.corrupted_files_by_sha1_checksum.join(", "))
						      .toLocal8Bit());
				}
				if (checkResult.corrupted_files_by_md5_checksum.length())
				{
					qCritical() << "Corrupted files in torrent, MD5 hash mismatch:" << checkResult.corrupted_files_by_md5_checksum.join(", ");
					logFile.write(QString("%1\t: ERROR, corrupted files in torrent, MD5 hash mismatch: %2\n")
						      .arg(torrent_file)
						      .arg(checkResult.corrupted_files_by_md5_checksum.join(", "))
						      .toLocal8Bit());
				}
				if (!continueOnErrorsFlag)
				{
					qCritical() << "Aborting torrent processing.";
					break;
				}
				logFile.write(logFileLineDelimiter);
				qCritical() << "----------------------------------------------------";
			}
			else
				logFile.write(QString("%1\t: OK\n").arg(torrent_file).toLocal8Bit());
		}

		logFile.flush();
	}
	logFile.write(logFileLineDelimiter);
	logFile.write("\n\n");
	qInfo() << "";
	if (checkResults.length())
	{
		/* Print long list of corrupted torrents. */
		logFile.write(logFileLineDelimiter);
		logFile.write("!!! ERROR !!! ERROR !!! ERROR !!!\n");
		logFile.write(logFileLineDelimiter);
		logFile.write("Corrupted torrent data found! The data for the following torrents is corrupted:\n");
		logFile.write(logFileLineDelimiter);
		qCritical() << "Corrupted torrent data found! The data for the following torrents is corrupted:";
		qCritical() << "----------------------------------------------------";
		for (const auto & t : checkResults)
		{
			qCritical().noquote() << t.torrent_filename;
			logFile.write(t.torrent_filename.toLocal8Bit() + '\n');
			if (t.corrupted_files_by_sha1_checksum.length())
			{
				qCritical().noquote() << "\tSHA1 corrupted files:";
				logFile.write("\tSHA1 corrupted files:\n");
				for (const auto & f : t.corrupted_files_by_sha1_checksum)
				{
					qCritical().noquote() << "\t" << f;
					logFile.write(QString("\t%1\n").arg(f).toLocal8Bit());
				}
			}
			if (t.corrupted_files_by_md5_checksum.length())
			{
				qCritical().noquote() << "\tMD5 corrupted files:";
				logFile.write("\tMD5 corrupted files:\n");
				for (const auto & f : t.corrupted_files_by_md5_checksum)
				{
					qCritical().noquote() << "\t" << f;
					logFile.write(QString("\t%1\n").arg(f).toLocal8Bit());
				}
			}
			logFile.write(logFileLineDelimiter);
			qCritical() << "----------------------------------------------------";
		}
		logFile.write("\n\n");
		logFile.flush();
		qCritical() << "";

		/* Print short list of corrupted torrents. */
		logFile.write(logFileLineDelimiter);
		logFile.write("!!! ERROR !!! ERROR !!! ERROR !!!\n");
		logFile.write(logFileLineDelimiter);
		logFile.write("Summary: the data for the following torrents is corrupted:\n");
		logFile.write(logFileLineDelimiter);
		qCritical() << "Summary: the data for the following torrents is corrupted:";
		qCritical() << "----------------------------------------------------";
		for (const auto & t : checkResults)
		{
			qCritical().noquote() << t.torrent_filename;
			logFile.write(t.torrent_filename.toLocal8Bit() + '\n');
		}
		logFile.write(logFileLineDelimiter);
		logFile.write("\n\n");
		logFile.flush();
		qCritical() << "----------------------------------------------------";
		qCritical() << "";


	}
	qInfo().noquote() << "Total torrents processed:" << total_torrents_processed << QString("(%1 corrupted)").arg(checkResults.length());
	qInfo() << "Total file count:" << total_file_count;
	qInfo() << "Total data size:" << total_length << "bytes," << (double) total_length / (1024 * 1024 * 1024) << "gigabytes," << ((double) total_length / (1024 * 1024 * 1024)) / 1024 << "terabytes";

	qint64 elapsed_time_ms = timer.elapsed();

	if (!dumpOnlyFlag)
		qInfo().noquote() << QString("%1 seconds (%2 hours) elapsed")
				     .arg(elapsed_time_ms / 1000).arg((double) elapsed_time_ms / (3600 * 1000), 0, 'f', 2);
	logFile.write(QString("Total torrents processed: %1 (%2 corrupted)\n").arg(total_torrents_processed).arg(checkResults.length()).toLocal8Bit());
	logFile.write(QString("Total file count: %1\n").arg(total_file_count).toLocal8Bit());
	logFile.write(QString("Total data size: %1 bytes, %2 gigabytes, %3 terabytes\n")
		      .arg(total_length)
		      .arg((double) total_length / (1024 * 1024 * 1024), 0, 'f', 2)
		      .arg(((double) total_length / (1024 * 1024 * 1024)) / 1024, 0, 'f', 2).toLocal8Bit());
	if (!dumpOnlyFlag)
		logFile.write(QString("Processing took %1 seconds (%2 hours)\n")
			      .arg(elapsed_time_ms / 1000)
			      .arg((double) elapsed_time_ms / (3600 * 1000), 0, 'f', 2).toLocal8Bit());
	if (!dumpOnlyFlag)
		logFile.write(QString("Average data read rate: %1 megabytes/second\n")
			      .arg(((double) total_length / elapsed_time_ms) * 1000. / (1024 * 1024), 0, 'f', 2).toLocal8Bit());
	logFile.close();
	return 0;
}
