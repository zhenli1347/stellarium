#include "DescriptionOldLoader.hpp"

#include <deque>
#include <QDir>
#include <QFile>
#include <QDebug>
#include <QRegularExpression>
#include <gettext-po.h>

namespace
{

#if (QT_VERSION>=QT_VERSION_CHECK(5, 14, 0))
constexpr auto SkipEmptyParts = Qt::SkipEmptyParts;
#else
constexpr auto SkipEmptyParts = QString::SkipEmptyParts;
#endif

const QRegularExpression htmlImageRegex(R"reg(<img\b[^>/]*(?:\s+alt="([^"]+)")?\s+src="([^"]+)"(?:\s+alt="([^"]+)")?\s*/?>)reg");

void htmlListsToMarkdown(QString& string)
{
	// This will only handle lists whose entries don't contain HTML tags, and the
	// lists don't contain anything except <li> entries (in particular, no comments).
	static const QRegularExpression ulistPattern(R"reg(<ul\s*>\s*(?:<li\s*>[^<]+</li\s*>\s*)+</ul\s*>)reg");
	static const QRegularExpression outerUListTagPattern(R"reg(</?ul\s*>)reg");
	static const QRegularExpression entryPattern(R"reg(<li\s*>([^<]+)</li\s*>)reg");
	for(auto matches = ulistPattern.globalMatch(string); matches.hasNext(); )
	{
		const auto& match = matches.next();
		auto list = match.captured(0);
		list.replace(outerUListTagPattern, "\n");
		list.replace(entryPattern, "\n - \\1\n");
		string.replace(match.captured(0), list);
	}
}

void htmlTablesToMarkdown(QString& string)
{
	// Using a single regex to find all tables without merging them into
	// one capture appears to be too hard. Let's go in a lower-level way,
	// by finding all the beginnings and ends manually.
	const QRegularExpression tableBorderPattern(R"reg((<table\b[^>]*>)|(</table\s*>))reg");
	bool foundStart = false;
	int startPos = -1, tagStartPos = -1;
	QStringList tables;
	std::vector<std::pair<int,int>> tablesPositions;
	for(auto matches = tableBorderPattern.globalMatch(string); matches.hasNext(); )
	{
		const auto& match = matches.next();
		const auto& startCap = match.captured(1);
		const auto& endCap = match.captured(2);
		if(!startCap.isEmpty() && endCap.isEmpty() && !foundStart)
		{
			foundStart = true;
			tagStartPos = match.capturedStart(1);
			startPos = match.capturedEnd(1);
		}
		else if(startCap.isEmpty() && !endCap.isEmpty() && foundStart)
		{
			foundStart = false;
			Q_ASSERT(startPos >= 0);
			Q_ASSERT(tagStartPos >= 0);
			const auto endPos = match.capturedStart(2);
			const auto tagEndPos = match.capturedEnd(2);
			tables += string.mid(startPos, endPos - startPos);
			tablesPositions.emplace_back(tagStartPos, tagEndPos);
		}
		else
		{
			qWarning() << "Inconsistency between table start and end tags detected, can't process tables further";
			return;
		}
	}

	// Now do the actual conversion
	for(int n = 0; n < tables.size(); ++n)
	{
		const auto& table = tables[n];
		if(table.contains(QRegularExpression("\\s(?:col|row)span=")))
		{
			qWarning() << "Row/column spans are not supported, leaving the table in HTML form";
			continue;
		}
		if(!table.contains(QRegularExpression("^\\s*<tr\\s*>")))
		{
			qWarning() << "Unexpected table contents (expected it to start with <tr>), keeping the table in HTML form";
			continue;
		}
		if(!table.contains(QRegularExpression("</tr\\s*>\\s*$")))
		{
			qWarning() << "Unexpected table contents (expected it to end with </tr>), keeping the table in HTML form";
			continue;
		}
		auto rows = table.split(QRegularExpression("\\s*</tr\\s*>\\s*"), SkipEmptyParts);
		// The closing row tags have been removed by QString::split, now remove the opening tags
		static const QRegularExpression trOpenTag("^\\s*<tr\\s*>\\s*");
		for(auto& row : rows) row.replace(trOpenTag, "");

		QString markdownTable;
		// Now convert the rows
		for(const auto& row : rows)
		{
			if(row.simplified().isEmpty()) continue;
			if(!row.contains(QRegularExpression("^\\s*<t[dh]\\s*>")))
			{
				qWarning() << "Unexpected row contents (expected it to start with <td> or <th>), keeping the table in HTML form. Row:" << row;
				goto nextTable;
			}
			if(!row.contains(QRegularExpression("</t[dh]\\s*>\\s*$")))
			{
				qWarning() << "Unexpected row contents (expected it to end with </td> or </th>), keeping the table in HTML form. Row:" << row;
				goto nextTable;
			}
			auto cols = row.split(QRegularExpression("\\s*</t[dh]\\s*>\\s*"), SkipEmptyParts);
			// The closing column tags have been removed by QString::split, now remove the opening tags
			static const QRegularExpression tdOpenTag("^\\s*<t[dh]\\s*>\\s*");
			for(auto& col : cols) col.replace(tdOpenTag, "");

			// Finally, emit the rows
			const bool firstRow = markdownTable.isEmpty();
			if(firstRow) markdownTable += "\n"; // make sure the table starts as a new paragraph
			markdownTable += "|";
			for(const auto& col : cols)
			{
				if(col.isEmpty())
					markdownTable += "   ";
				else
					markdownTable += col;
				markdownTable += '|';
			}
			markdownTable += '\n';
			if(firstRow)
			{
				markdownTable += '|';
				for(const auto& col : cols)
				{
					markdownTable += QString(std::max(3, col.size()), QChar('-'));
					markdownTable += '|';
				}
				markdownTable += "\n";
			}
		}

		// Replace the HTML table with the newly-created Markdown one
		{
			const auto lengthToReplace = tablesPositions[n].second - tablesPositions[n].first;
			string.replace(tablesPositions[n].first, lengthToReplace, markdownTable);
			// Fixup the positions of the subsequent tables
			const int delta = markdownTable.size() - lengthToReplace;
			for(auto& positions : tablesPositions)
			{
				positions.first += delta;
				positions.second += delta;
			}
		}

nextTable:
		continue;
	}

	// Format the tables that we've failed to convert with each row
	// on its line, and each column entry on an indented line.
	string.replace(QRegularExpression("(<tr(?:\\s+[^>]*)*>)"), "\n\\1");
	string.replace(QRegularExpression("(</tr\\s*>)"), "\n\\1");
	string.replace(QRegularExpression("(<td(?:\\s+[^>]*)*>)"), "\n\t\\1");
	string.replace(QRegularExpression("(</table\\s*>)"), "\n\\1");
}

QString readReferencesFile(const QString& inDir)
{
	const auto path = inDir + "/reference.fab";
	if (path.isEmpty())
	{
		qWarning() << "Reference file wasn't found";
		return "";
	}
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
	{
		qWarning() << "WARNING - could not open" << QDir::toNativeSeparators(path);
		return "";
	}
	QString record;
	// Allow empty and comment lines where first char (after optional blanks) is #
	static const QRegularExpression commentRx("^(\\s*#.*|\\s*)$");
	QString reference = "## References\n\n";
	int totalRecords=0;
	int readOk=0;
	int lineNumber=0;
	while(!file.atEnd())
	{
		record = QString::fromUtf8(file.readLine()).trimmed();
		lineNumber++;
		if (commentRx.match(record).hasMatch())
			continue;

		totalRecords++;
		static const QRegularExpression refRx("\\|");
		#if (QT_VERSION>=QT_VERSION_CHECK(5, 14, 0))
		QStringList ref = record.split(refRx, Qt::KeepEmptyParts);
		#else
		QStringList ref = record.split(refRx, QString::KeepEmptyParts);
		#endif
		// 1 - URID; 2 - Reference; 3 - URL (optional)
		if (ref.count()<2)
			qWarning() << "Error: cannot parse record at line" << lineNumber << "in references file" << QDir::toNativeSeparators(path);
		else if (ref.count()<3)
		{
			qWarning() << "Warning: record at line" << lineNumber << "in references file"
			           << QDir::toNativeSeparators(path) << " has wrong format (RefID: " << ref.at(0) << ")! Let's use fallback mode...";
			reference.append(QString(" %1. %2\n").arg(ref[0], ref[1]));
			readOk++;
		}
		else
		{
			if (ref.at(2).isEmpty())
				reference.append(QString(" %1. %2\n").arg(ref[0], ref[1]));
			else
				reference.append(QString(" %1. [%2](%3)\n").arg(ref[0], ref[1], ref[2]));
			readOk++;
		}
	}
	if(readOk != totalRecords)
		qDebug() << "Loaded" << readOk << "/" << totalRecords << "references";

	return reference;
}

void cleanupWhitespace(QString& markdown)
{
	// Clean too long chains of newlines
	markdown.replace(QRegularExpression("\n\n\n+"), "\n\n");

	// Remove trailing spaces
	markdown.replace(QRegularExpression("[ \t]+\n"), "\n");

	// Make unordered lists a bit denser
	const QRegularExpression ulistSpaceListPattern("(\n -[^\n]+)\n+(\n \\-)");
	//  1. Remove space between odd and even entries
	markdown.replace(ulistSpaceListPattern, "\\1\\2");
	//  2. Remove space between even and odd entries (same replacement rule)
	markdown.replace(ulistSpaceListPattern, "\\1\\2");

	// Make ordered lists a bit denser
	const QRegularExpression olistSpaceListPattern("(\n 1\\.[^\n]+)\n+(\n 1)");
	//  1. Remove space between odd and even entries
	markdown.replace(olistSpaceListPattern, "\\1\\2");
	//  2. Remove space between even and odd entries (same replacement rule)
	markdown.replace(olistSpaceListPattern, "\\1\\2");

	markdown = markdown.trimmed() + "\n";
}

[[nodiscard]] QString convertHTMLToMarkdown(const QString& html)
{
	QString markdown = html;

	markdown.replace(QRegularExpression("[\n\t ]+"), " ");

	// Replace <notr> and </notr> tags with placeholders that don't
	// look like tags, so as not to confuse the replacements below.
	const QString notrOpenPlaceholder = "[22c35d6a-5ec3-4405-aeff-e79998dc95f7]";
	const QString notrClosePlaceholder = "[2543be41-c785-4283-a4cf-ce5471d2c422]";
	markdown.replace(QRegularExpression("<notr\\s*>"), notrOpenPlaceholder);
	markdown.replace(QRegularExpression("</notr\\s*>"), notrClosePlaceholder);

	// Replace simple HTML headings with corresponding Markdown ones
	for(int n = 1; n <= 6; ++n)
		markdown.replace(QRegularExpression(QString("<h%1(?:\\s+[^>]*)*>([^<]+)</h%1>").arg(n)), "\n" + QString(n, QChar('#'))+" \\1\n");

	// Replace HTML line breaks with the Markdown ones
	markdown.replace(QRegularExpression("<br\\s*/?>"), "\n\n");

	// Replace simple HTML italics with the Markdown ones
	markdown.replace(QRegularExpression("<i>\\s*([^<]+)\\s*</i>"), "*\\1*");
	markdown.replace(QRegularExpression("<em>\\s*([^<]+)\\s*</em>"), "*\\1*");

	// Replace simple HTML images with the Markdown ones
	markdown.replace(htmlImageRegex, R"rep(![\1\3](\2))rep");

	// Replace simple HTML hyperlinks with the Markdown ones
	markdown.replace(QRegularExpression("([^>])\\s*<a\\s+href=\"([^\"]+)\"(?:\\s[^>]*)?>([^<]+)</a\\s*>\\s*([^<])"), "\\1[\\3](\\2)\\4");

	// Replace simple HTML paragraphs with the Markdown ones
	markdown.replace(QRegularExpression("<p>([^<]+)</p>"), "\n\\1\n");
	// Place those paragraphs that we haven't handled each on a separate line
	markdown.replace(QRegularExpression("(<p(?:\\s+[^>]*)*>)"), "\n\\1");

	htmlTablesToMarkdown(markdown);

	htmlListsToMarkdown(markdown);

	cleanupWhitespace(markdown);

	// Restore <notr> and </notr> tags
	markdown.replace(notrOpenPlaceholder,  "<notr>");
	markdown.replace(notrClosePlaceholder, "</notr>");

	return markdown;
}

void addMissingTextToMarkdown(QString& markdown, const QString& inDir, const QString& author, const QString& credit, const QString& license)
{
	// Add missing "Introduction" heading if we have a headingless intro text
	if(!markdown.contains(QRegularExpression("^\\s*# [^\n]+\n+\\s*##\\s*Introduction\n")))
	{
		markdown.replace(QRegularExpression("^(\\s*# [^\n]+\n+)(\\s*[^#])"), "\\1## Introduction\n\n\\2");
		markdown.replace(QRegularExpression("(\n## Introduction\n[^#]+\n)(\\s*#)"), "\\1## Description\n\n\\2");
	}

	// Add some sections the info for which is contained in info.ini in the old format
	if(markdown.contains(QRegularExpression("\n##\\s+(?:References|External\\s+links)\\s*\n")))
		markdown.replace(QRegularExpression("(\n##[ \t]+)External[ \t]+links([ \t]*\n)"), "\\1References\\2");
	auto referencesFromFile = readReferencesFile(inDir);

	if(markdown.contains(QRegularExpression("\n##\\s+Authors?\\s*\n")))
	{
		qWarning() << "Authors section already exists, not adding the authors from info.ini";

		// But do add references before this section
		if(!referencesFromFile.isEmpty())
			markdown.replace(QRegularExpression("(\n##\\s+Authors?\\s*\n)"), "\n"+referencesFromFile + "\n\\1");
	}
	else
	{
		// First add references
		if(!referencesFromFile.isEmpty())
			markdown += referencesFromFile + "\n";

		if(credit.isEmpty())
			markdown += QString("\n## Authors\n\n%1\n").arg(author);
		else
			markdown += "\n## Authors\n\nAuthor is " + author + ". Additional credit goes to " + credit + "\n";
	}

	if(markdown.contains(QRegularExpression("\n##\\s+License\\s*\n")))
		qWarning() << "License section already exists, not adding the license from info.ini";
	else
		markdown += "\n## License\n\n" + license + "\n";

	cleanupWhitespace(markdown);
}

struct Section
{
	int level = -1;
	int levelAddition = 0;
	int headerLineStartPos = -1;
	int headerStartPos = -1; // including #..#
	int bodyStartPos = -1;
	QString title;
	QString body;
	std::deque<int> subsections;
};

std::vector<Section> splitToSections(const QString& markdown)
{
	const QRegularExpression sectionHeaderPattern("^\\s*((#+)\\s+(.*[^\\s])\\s*)$", QRegularExpression::MultilineOption);
	std::vector<Section> sections;
	for(auto matches = sectionHeaderPattern.globalMatch(markdown); matches.hasNext(); )
	{
		sections.push_back({});
		auto& section = sections.back();
		const auto& match = matches.next();
		section.headerLineStartPos = match.capturedStart(0);
		section.headerStartPos = match.capturedStart(1);
		section.level = match.captured(2).length();
		section.title = match.captured(3);
		section.bodyStartPos = match.capturedEnd(0) + 1/*\n*/;

		if(section.title.trimmed() == "Author")
			section.title = "Authors";
	}

	for(unsigned n = 0; n < sections.size(); ++n)
	{
		if(n+1 < sections.size())
			sections[n].body = markdown.mid(sections[n].bodyStartPos,
			                                sections[n+1].headerLineStartPos - sections[n].bodyStartPos);
		else
			sections[n].body = markdown.mid(sections[n].bodyStartPos);
	}

	return sections;
}

bool isStandardTitle(const QString& title)
{
	return title == "Introduction" ||
	       title == "Description" ||
	       title == "Constellations" ||
	       title == "References" ||
	       title == "Authors" ||
	       title == "License";
}

void gettextpo_xerror(int severity, po_message_t message, const char *filename, size_t lineno, size_t column, int multiline_p, const char *message_text)
{
	(void)message;
	qWarning().nospace() << "libgettextpo: " << filename << ":" << lineno << ":" << column << ": " << (multiline_p ? "\n" : "") << message_text;
	if(severity == PO_SEVERITY_FATAL_ERROR)
		std::abort();
}

void gettextpo_xerror2(int severity,
                       po_message_t message1, const char *filename1, size_t lineno1, size_t column1, int multiline_p1, const char *message_text1,
                       po_message_t message2, const char *filename2, size_t lineno2, size_t column2, int multiline_p2, const char *message_text2)
{
	(void)message1;
	(void)message2;
	qWarning().nospace() << "libgettextpo: error with two messages:";
	qWarning().nospace() << "libgettextpo: message 1 error: " << filename1 << ":" << lineno1 << ":" << column1 << ": " << (multiline_p1 ? "\n" : "") << message_text1;
	qWarning().nospace() << "libgettextpo: message 2 error: " << filename2 << ":" << lineno2 << ":" << column2 << ": " << (multiline_p2 ? "\n" : "") << message_text2;
	if(severity == PO_SEVERITY_FATAL_ERROR)
		std::abort();
}

}

void DescriptionOldLoader::loadTranslationsOfNames(const QString& poDir, const QString& cultureIdQS)
{
	po_xerror_handler handler = {gettextpo_xerror, gettextpo_xerror2};
	const auto cultureId = cultureIdQS.toStdString();

	for(const auto& fileName : QDir(poDir).entryList({"*.po"}))
	{
		const QString locale = fileName.chopped(3);
		const auto file = po_file_read((poDir+"/"+fileName).toStdString().c_str(), &handler);
		if(!file) continue;

		const auto header = po_file_domain_header(file, nullptr);
		if(header) poHeaders[locale] = header;

		const auto domains = po_file_domains(file);
		for(auto domainp = domains; *domainp; domainp++)
		{
			const auto domain = *domainp;
			po_message_iterator_t iterator = po_message_iterator(file, domain);

			for(auto message = po_next_message(iterator); message != nullptr; message = po_next_message(iterator))
			{
				const auto msgid = po_message_msgid(message);
				const auto msgstr = po_message_msgstr(message);
				const auto comments = po_message_extracted_comments(message);
				for(int n = 0; ; ++n)
				{
					const auto filepos = po_message_filepos(message, n);
					if(!filepos) break;
					const auto refFileName = po_filepos_file(filepos);
					for(const auto ref : {
					                      "skycultures/"+cultureId+"/star_names.fab",
					                      "skycultures/"+cultureId+"/dso_names.fab",
					                      "skycultures/"+cultureId+"/planet_names.fab",
					                      "skycultures/"+cultureId+"/asterism_names.fab",
					                      "skycultures/"+cultureId+"/constellation_names.eng.fab",
					                      })
					{
						if(refFileName == ref)
							translations[locale].push_back({comments, msgid, msgstr});
					}
				}
			}
			po_message_iterator_free(iterator);
		}
		po_file_free(file);
	}
}

void DescriptionOldLoader::locateAllInlineImages(const QString& html)
{
	for(auto matches = htmlImageRegex.globalMatch(html); matches.hasNext(); )
	{
		const auto& match = matches.next();
		imageHRefs.emplace_back(match.captured(2));
	}
}

void DescriptionOldLoader::load(const QString& inDir, const QString& poDir, const QString& cultureId,
                                const QString& author, const QString& credit, const QString& license)
{
	inputDir = inDir;
	const auto englishDescrPath = inDir+"/description.en.utf8";
	QFile englishDescrFile(englishDescrPath);
	if(!englishDescrFile.open(QFile::ReadOnly))
	{
		qCritical().noquote() << "Failed to open file" << englishDescrPath;
		return;
	}
	const auto html = englishDescrFile.readAll();
	locateAllInlineImages(html);
	markdown = convertHTMLToMarkdown(html);

	auto englishSections = splitToSections(markdown);
	const int level1sectionCount = std::count_if(englishSections.begin(), englishSections.end(),
	                                             [](auto& s){return s.level==1;});
	if(level1sectionCount != 1)
	{
		qCritical().nospace() << "Too many level-1 sections in file " << englishDescrFile
		                      << " (expected 1, found " << level1sectionCount
		                      << "), will not convert the description";
		return;
	}

	// Mark all sections with level>2 to be subsections of the nearest preceding level<=2 sections
	std::deque<int> subsections;
	for(int n = signed(englishSections.size()) - 1; n >= 0; --n)
	{
		if(englishSections[n].level > 2 || (englishSections[n].level == 2 && !isStandardTitle(englishSections[n].title)))
		{
			subsections.push_front(n);
		}
		else
		{
			englishSections[n].subsections = std::move(subsections);
			subsections.clear();
		}
	}

	// Increase the level of all level-2 sections and their subsections unless they have one of the standard titles
	for(auto& section : englishSections)
	{
		if(section.level != 2 || isStandardTitle(section.title)) continue;
		if(section.level == 2)
		{
			for(const int n : section.subsections)
				englishSections[n].levelAddition = 1;
		}
		section.levelAddition = 1;
	}

	if(englishSections.empty())
	{
		qCritical() << "No sections found in" << englishDescrPath;
		return;
	}

	if(englishSections[0].level != 1)
	{
		qCritical() << "Unexpected section structure: first section must have level 1, but instead has" << englishSections[0].level;
		return;
	}

	const QRegularExpression localePattern("description\\.([^.]+)\\.utf8");
	for(const auto& fileName : QDir(inDir).entryList({"description.*.utf8"}))
	{
		if(fileName == "description.en.utf8") continue;

		const auto localeMatch = localePattern.match(fileName);
		if(!localeMatch.isValid())
		{
			qCritical() << "Failed to extract locale from file name" << fileName;
			continue;
		}
		const auto locale = localeMatch.captured(1);
		const auto path = inDir + "/" + fileName;
		QFile file(path);
		if(!file.open(QFile::ReadOnly))
		{
			qCritical().noquote() << "Failed to open file" << path << "\n";
			continue;
		}
		const auto translationMD = convertHTMLToMarkdown(file.readAll()).replace(QRegularExpression("<notr>([^<]+)</notr>"), "\\1");
		const auto translatedSections = splitToSections(translationMD);
		if(translatedSections.size() != englishSections.size())
		{
			qCritical().nospace().noquote() << "Number of sections (" << translatedSections.size()
			                                << ") in description for locale " << locale
			                                << " doesn't match that of the English description ("
			                                << englishSections.size() << "). Skipping this translation.";
			continue;
		}

		for(unsigned n = 0; n < englishSections.size(); ++n)
		{
			if(translatedSections[n].level != englishSections[n].level)
			{
				qCritical() << "Section structure of English text and translation for"
				            << locale << "doesn't match, skipping this translation";
				continue;
			}
		}

		TranslationDict dict;
		dict.push_back({"Sky culture name", englishSections[0].title, translatedSections[0].title});
		for(unsigned n = 0; n < englishSections.size(); ++n)
		{
			const auto& engSec = englishSections[n];
			if(engSec.level + engSec.levelAddition > 2) continue;

			QString key = engSec.body.trimmed();
			QString value = translatedSections[n].body.trimmed();
			auto titleForComment = engSec.title.contains(' ') ? '"' + engSec.title.toLower() + '"' : engSec.title.toLower();
			if(engSec.level == 1)
			{
				auto comment = QString("Sky culture introduction section in markdown format");
				dict.push_back({std::move(comment), std::move(key), std::move(value)});
				key = "";
				value = "";
				titleForComment = "description";
			}

			for(const auto subN : engSec.subsections)
			{
				const auto& keySubSection = englishSections[subN];
				key += "\n\n";
				key += QString(keySubSection.level + keySubSection.levelAddition, QChar('#'));
				key += ' ';
				key += keySubSection.title;
				key += "\n\n";
				key += keySubSection.body;
				key += "\n\n";
				cleanupWhitespace(key);
				key = key.trimmed();

				const auto& valueSubSection = translatedSections[subN];
				value += "\n\n";
				value += QString(keySubSection.level + keySubSection.levelAddition, QChar('#'));
				value += ' ';
				value += valueSubSection.title;
				value += "\n\n";
				value += valueSubSection.body;
				value += "\n\n";
				cleanupWhitespace(value);
				value = value.trimmed();
			}
			auto comment = QString("Sky culture %1 section in markdown format").arg(titleForComment);
			if(!key.isEmpty())
				dict.push_back({std::move(comment), std::move(key), std::move(value)});
		}
		translations[locale] = std::move(dict);
	}

	// Reconstruct markdown from the altered sections
	markdown.clear();
	for(const auto& section : englishSections)
	{
		markdown += QString(section.level + section.levelAddition, QChar('#'));
		markdown += ' ';
		markdown += section.title.trimmed();
		markdown += "\n\n";
		if(section.body.startsWith(" 1. ") || section.body.startsWith(" - "))
			markdown += ' '; // undo trimming effect on lists
		markdown += section.body.trimmed();
		markdown += "\n\n";
	}

	addMissingTextToMarkdown(markdown, inDir, author, credit, license);

	loadTranslationsOfNames(poDir, cultureId);
}

bool DescriptionOldLoader::dumpMarkdown(const QString& outDir) const
{
	const auto path = outDir+"/description.md";
	QFile file(path);
	if(!file.open(QFile::WriteOnly))
	{
		qCritical().noquote() << "Failed to open file" << path << "\n";
		return false;
	}

	if(markdown.isEmpty()) return true;

	if(file.write(markdown.toUtf8()) < 0 || !file.flush())
	{
		qCritical().noquote() << "Failed to write " << path << ": " << file.errorString() << "\n";
		return false;
	}

	for(const auto& img : imageHRefs)
	{
		const auto imgInPath = inputDir+"/"+img;
		if(!QFileInfo(imgInPath).exists())
		{
			qCritical() << "Failed to locate an image referenced in the description:" << img;
			continue;
		}
		const auto imgOutPath = outDir + "/" + img;
		const auto imgDir = QFileInfo(imgOutPath).absoluteDir().absolutePath();
		if(!QDir().mkpath(imgDir))
		{
			qCritical() << "Failed to create output directory for image file" << img;
			continue;
		}

		if(!QFile(imgInPath).copy(imgOutPath))
		{
			qCritical() << "Failed to copy an image file referenced in the description:" << img;
			continue;
		}
	}

	return true;
}

bool DescriptionOldLoader::dump(const QString& outDir) const
{
	if(!dumpMarkdown(outDir)) return false;

	const auto poDir = outDir + "/po";
	if(!QDir().mkpath(poDir))
	{
		qCritical() << "Failed to create po directory\n";
		return false;
	}

	for(auto dictIt = translations.begin(); dictIt != translations.end(); ++dictIt)
	{
		const auto& locale = dictIt.key();
		const auto path = poDir + "/" + locale + ".po";

		const auto file = po_file_create();
		po_message_iterator_t iterator = po_message_iterator(file, nullptr);

		// I've found no API to *create* a header, so will try to emulate it with a message
		const auto header = poHeaders[locale];
		const auto headerMsg = po_message_create();
		po_message_set_msgid(headerMsg, "");
		po_message_set_msgstr(headerMsg, header.toStdString().c_str());
		po_message_insert(iterator, headerMsg);

		for(const auto& entry : dictIt.value())
		{
			const auto msg = po_message_create();
			po_message_set_extracted_comments(msg, entry.comment.toStdString().c_str());
			po_message_set_msgid(msg, entry.english.toStdString().c_str());
			po_message_set_msgstr(msg, entry.translated.toStdString().c_str());
			po_message_insert(iterator, msg);
		}
		po_message_iterator_free(iterator);
		po_xerror_handler handler = {gettextpo_xerror, gettextpo_xerror2};
		po_file_write(file, path.toStdString().c_str(), &handler);
		po_file_free(file);
	}
	return true;
}
