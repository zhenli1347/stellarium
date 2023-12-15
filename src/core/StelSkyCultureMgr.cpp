/*
 * Stellarium
 * Copyright (C) 2006 Fabien Chereau
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335, USA.
 */

#include "StelSkyCultureMgr.hpp"
#include "StelFileMgr.hpp"
#include "StelTranslator.hpp"
#include "StelLocaleMgr.hpp"
#include "StelApp.hpp"
#include "StelIniParser.hpp"

#include <md4c-html.h>

#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QDebug>
#include <QMap>
#include <QMapIterator>
#include <QDir>
#include <QJsonObject>
#include <QJsonDocument>
#include <QRegularExpression>

namespace
{

#if (QT_VERSION>=QT_VERSION_CHECK(5, 14, 0))
constexpr auto SkipEmptyParts = Qt::SkipEmptyParts;
#else
constexpr auto SkipEmptyParts = QString::SkipEmptyParts;
#endif

QString markdownToHTML(QString input)
{
	const auto inputUTF8 = input.toStdString();

	std::string outputUTF8;
	::md_html(inputUTF8.data(), inputUTF8.size(),
	          [](const char* html, const MD_SIZE size, void* output)
	          { static_cast<std::string*>(output)->append(html, size); },
	          &outputUTF8, MD_DIALECT_GITHUB, 0);

	return QString::fromStdString(outputUTF8);
}

QString convertReferenceLinks(QString text)
{
	text.replace(QRegularExpression(" ?\\[#([0-9]+)\\]", QRegularExpression::MultilineOption),
	             "<sup><a href=\"#cite_\\1\">[\\1]</a></sup>");
	return text;
}

}

QString StelSkyCultureMgr::getSkyCultureEnglishName(const QString& idFromJSON) const
{
	const auto skyCultureId = idFromJSON;
	const QString descPath = StelFileMgr::findFile("skycultures/" + skyCultureId + "/description.md");
	if (descPath.isEmpty())
	{
		qWarning() << "WARNING: can't find description for skyculture" << skyCultureId;
		return idFromJSON;
	}

	QFile f(descPath);
	if (!f.open(QIODevice::ReadOnly))
	{
		qWarning().nospace() << "Failed to open sky culture description file " << descPath << ": " << f.errorString();
		return idFromJSON;
	}

	for (int lineNum = 1;; ++lineNum)
	{
		const auto line = QString::fromUtf8(f.readLine()).trimmed();
		if (line.isEmpty()) continue;
		if (!line.startsWith("#"))
		{
			qWarning().nospace() << "Sky culture description file " << descPath << " at line "
			                     << lineNum << " has wrong format (expected a top-level header, got " << line;
			return idFromJSON;
		}
		return line.mid(1).trimmed();
	}

	qWarning() << "Failed to find sky culture name in" << descPath;
	return idFromJSON;
}

StelSkyCultureMgr::StelSkyCultureMgr()
{
	setObjectName("StelSkyCultureMgr");
	makeCulturesList();
}

StelSkyCultureMgr::~StelSkyCultureMgr()
{
}

void StelSkyCultureMgr::makeCulturesList()
{
	QSet<QString> cultureDirNames = StelFileMgr::listContents("skycultures",StelFileMgr::Directory);
	for (const auto& dir : std::as_const(cultureDirNames))
	{
		constexpr char indexFileName[] = "/index.json";
		const QString filePath = StelFileMgr::findFile("skycultures/" + dir + indexFileName);
		if (filePath.isEmpty())
		{
			qCritical() << "Failed to find" << indexFileName << "file in sky culture directory" << QDir::toNativeSeparators(dir);
			continue;
		}
		QFile file(filePath);
		if (!file.open(QFile::ReadOnly))
		{
			qCritical() << "Failed to open" << indexFileName << "file in sky culture directory" << QDir::toNativeSeparators(dir);
			continue;
		}
		const auto jsonText = file.readAll();
		if (jsonText.isEmpty())
		{
			qCritical() << "Failed to read data from" << indexFileName << "file in sky culture directory"
			           << QDir::toNativeSeparators(dir);
			continue;
		}
		QJsonParseError error;
		const auto jsonDoc = QJsonDocument::fromJson(jsonText, &error);
		if (error.error != QJsonParseError::NoError)
		{
			qCritical().nospace() << "Failed to parse " << indexFileName << " from sky culture directory "
			                     << QDir::toNativeSeparators(dir) << ": " << error.errorString();
			continue;
		}
		if (!jsonDoc.isObject())
		{
			qCritical() << "Failed to find the expected JSON structure in" << indexFileName << " from sky culture directory"
			           << QDir::toNativeSeparators(dir);
			continue;
		}
		const auto data = jsonDoc.object();

		auto& culture = dirToNameEnglish[dir];
		culture.dirName = dir;
		const auto id = data["id"].toString();
		if(id != dir)
			qWarning() << "Sky culture id" << id << "doesn't match directory name" << dir;
		culture.englishName = getSkyCultureEnglishName(dir);
		culture.region = data["region"].toString();
		if (data["constellations"].isArray())
		{
			culture.constellations = data["constellations"].toArray();
		}
		else
		{
			qWarning() << "No \"constellations\" array found in JSON data in sky culture directory"
			           << QDir::toNativeSeparators(dir);
		}

		culture.asterisms = data["asterisms"].toArray();

		culture.boundariesType = StelSkyCulture::BoundariesType::Own; // default value if not specified in the JSON file
		if (data.contains("edges"))
		{
			if (data.contains("edges_type"))
			{
				const auto type = data["edges_type"].toString();
				const auto typeSimp = type.simplified().toUpper();
				if (typeSimp == "IAU")
					culture.boundariesType = StelSkyCulture::BoundariesType::IAU;
				else if(typeSimp == "OWN")
					culture.boundariesType = StelSkyCulture::BoundariesType::Own;
				else if(typeSimp == "NONE")
					culture.boundariesType = StelSkyCulture::BoundariesType::None;
				else
					qWarning().nospace() << "Unexpected edges_type value in sky culture " << dir
					                     << ": " << type << ". Will resort to Own.";
			}
		}
		else
		{
			culture.boundariesType = StelSkyCulture::BoundariesType::None;
		}
		culture.boundaries = data["edges"].toArray();
		culture.boundariesEpoch = data["edges_epoch"].toString("J2000");
		culture.fallbackToInternationalNames = data["fallback_to_international_names"].toBool();
		culture.names = data["common_names"].toObject();

		const auto classifications = data["classification"].toArray();
		if (classifications.isEmpty())
		{
			culture.classification = StelSkyCulture::INCOMPLETE;
		}
		else
		{
			static const QMap <QString, StelSkyCulture::CLASSIFICATION>classificationMap={
				{ "traditional",  StelSkyCulture::TRADITIONAL},
				{ "historical",   StelSkyCulture::HISTORICAL},
				{ "ethnographic", StelSkyCulture::ETHNOGRAPHIC},
				{ "single",       StelSkyCulture::SINGLE},
				{ "comparative",  StelSkyCulture::COMPARATIVE},
				{ "personal",     StelSkyCulture::PERSONAL},
				{ "incomplete",   StelSkyCulture::INCOMPLETE},
			};
			const auto classificationStr = classifications[0].toString(); // We'll take only the first item for now.
			const auto classification=classificationMap.value(classificationStr.toLower(), StelSkyCulture::INCOMPLETE);
			if (classificationMap.constFind(classificationStr.toLower()) == classificationMap.constEnd()) // not included
			{
				qDebug() << "Skyculture " << dir << "has UNKNOWN classification: " << classificationStr;
				qDebug() << "Please edit info.ini and change to a supported value. For now, this equals 'incomplete'";
			}
			culture.classification = classification;
		}
	}
}

//! Init itself from a config file.
void StelSkyCultureMgr::init()
{
	defaultSkyCultureID = StelApp::getInstance().getSettings()->value("localization/sky_culture", "modern").toString();
	if (defaultSkyCultureID=="western") // switch to new Sky Culture ID
		defaultSkyCultureID = "modern";
	setCurrentSkyCultureID(defaultSkyCultureID);
}

void StelSkyCultureMgr::reloadSkyCulture()
{
	emit currentSkyCultureChanged(currentSkyCulture);
}

//! Set the current sky culture from the passed directory
bool StelSkyCultureMgr::setCurrentSkyCultureID(const QString& cultureDir)
{
	//prevent unnecessary changes
	if(cultureDir==currentSkyCultureDir)
		return false;

	QString scID = cultureDir;
	bool result = true;
	// make sure culture definition exists before attempting or will die
	if (directoryToSkyCultureEnglish(cultureDir) == "")
	{
		qWarning() << "Invalid sky culture directory: " << QDir::toNativeSeparators(cultureDir);
		scID = "modern";
		result = false;
	}

	currentSkyCultureDir = scID;
	currentSkyCulture = dirToNameEnglish[scID];

	emit currentSkyCultureChanged(currentSkyCulture);
	return result;
}

// Set the default sky culture from the ID.
bool StelSkyCultureMgr::setDefaultSkyCultureID(const QString& id)
{
	// make sure culture definition exists before attempting or will die
	if (directoryToSkyCultureEnglish(id) == "")
	{
		qWarning() << "Invalid sky culture ID: " << id;
		return false;
	}
	defaultSkyCultureID = id;
	QSettings* conf = StelApp::getInstance().getSettings();
	Q_ASSERT(conf);
	conf->setValue("localization/sky_culture", id);

	emit defaultSkyCultureChanged(id);
	return true;
}
	
QString StelSkyCultureMgr::getCurrentSkyCultureNameI18() const
{
	return qc_(currentSkyCulture.englishName, "sky culture");
}

QString StelSkyCultureMgr::getCurrentSkyCultureEnglishName() const
{
	return currentSkyCulture.englishName;
}

StelSkyCulture::BoundariesType StelSkyCultureMgr::getCurrentSkyCultureBoundariesType() const
{
	return currentSkyCulture.boundariesType;
}

int StelSkyCultureMgr::getCurrentSkyCultureClassificationIdx() const
{
	return currentSkyCulture.classification;
}

QString StelSkyCultureMgr::getCurrentSkyCultureHtmlClassification() const
{
	QString classification, description, color;
	switch (currentSkyCulture.classification)
	{
		case StelSkyCulture::ETHNOGRAPHIC:
			color = "#33ff33"; // "green" area
			classification = qc_("ethnographic", "sky culture classification");
			description = q_("Provided by ethnographic researchers based on interviews of indigenous people.");
			break;
		case StelSkyCulture::HISTORICAL:
			color = "#33ff33"; // "green" area
			classification = qc_("historical", "sky culture classification");
			description = q_("Provided by historians based on historical written sources from a (usually short) period of the past.");
			break;
		case StelSkyCulture::SINGLE:
			color = "#33ff33"; // "green" area
			classification = qc_("single", "sky culture classification");
			description = q_("Represents a single source like a historical atlas, or publications of a single author.");
			break;
		case StelSkyCulture::COMPARATIVE:
			color = "#2090ff"; // "blue" area
			classification = qc_("comparative", "sky culture classification");
			description = q_("Compares and confronts elements from at least two sky cultures with each other.");
			break;
		case StelSkyCulture::TRADITIONAL:
			color = "#33ff33"; // "green" area
			classification = qc_("traditional", "sky culture classification");
			description = q_("Content represents 'common' knowledge by several members of an ethnic community, and the sky culture has been developed by members of such community.");
			break;
		case StelSkyCulture::PERSONAL:
			color = "#ffff00"; // "yellow" area
			classification = qc_("personal", "sky culture classification");
			description = q_("This is a personally developed sky culture which is not founded in published historical or ethnological research. Stellarium may include it when it is 'pretty enough' without really approving its contents.");
			break;
		case StelSkyCulture::INCOMPLETE:
			color = "#ff6633"; // "red" area
			classification = qc_("incomplete", "sky culture classification");
			description = q_("The accuracy of the sky culture description cannot be given, although it looks like it is built on a solid background. More work would be needed.");
			break;
		default: // undefined
			color = "#ff00cc";
			classification = qc_("undefined", "sky culture classification");
			description = QString();
			break;
	}

	QString html = QString();
	if (!description.isEmpty()) // additional info for sky culture (metainfo): let's use italic
		html = QString("<dl><dt><span style='color:%4;'>%5</span> <strong>%1: %2</strong></dt><dd><em>%3</em></dd></dl>").arg(q_("Classification"), classification, description, color, QChar(0x25CF));

	return html;
}


std::pair<QString/*color*/,QString/*info*/> StelSkyCultureMgr::getLicenseDescription(const QString& license, const bool singleLicenseForAll) const
{
	QString color, description;

	if (license.isEmpty())
	{
		color = "#2090ff"; // "blue" area
		description = q_("This sky culture is provided under unknown license. Please ask authors for details about license for this sky culture.");
	}
	else if (license.contains("GPL", Qt::CaseSensitive))
	{
		color = "#33ff33"; // "green" area; free license
		if (singleLicenseForAll)
			description = q_("This sky culture is provided under GNU General Public License. You can use it for commercial "
			                 "and non-commercial purposes, freely adapt it and share adapted work.");
		else
			description = q_("You can use it for commercial and non-commercial purposes, freely adapt it and share adapted work.");
	}
	else if (license.contains("MIT", Qt::CaseSensitive))
	{
		color = "#33ff33"; // "green" area; free license
		if (singleLicenseForAll)
			description = q_("This sky culture is provided under MIT License. You can use it for commercial and non-commercial "
			                 "purposes, freely adapt it and share adapted work.");
		else
			description = q_("You can use it for commercial and non-commercial purposes, freely adapt it and share adapted work.");
	}
	else if (license.contains("Public Domain"))
	{
		color = "#33ff33"; // "green" area; free license
		if (singleLicenseForAll)
			description = q_("This sky culture is distributed as public domain.");
		else
			description = q_("This is distributed as public domain.");
	}
	else if (license.startsWith("CC", Qt::CaseSensitive) || license.contains("Creative Commons", Qt::CaseInsensitive))
	{
		if (singleLicenseForAll)
			description = q_("This sky culture is provided under Creative Commons License.");

		QStringList details = license.split(" ", SkipEmptyParts);

		const QMap<QString, QString>options = {
			{ "BY",       q_("You may distribute, remix, adapt, and build upon this sky culture, even commercially, as long "
			                 "as you credit authors for the original creation.") },
			{ "BY-SA",    q_("You may remix, adapt, and build upon this sky culture even for commercial purposes, as long as "
			                 "you credit authors and license the new creations under the identical terms. This license is often "
			                 "compared to “copyleft” free and open source software licenses.") },
			{ "BY-ND",    q_("You may reuse this sky culture for any purpose, including commercially; however, adapted work "
			                 "cannot be shared with others, and credit must be provided by you.") },
			{ "BY-NC",    q_("You may remix, adapt, and build upon this sky culture non-commercially, and although your new works "
			                 "must also acknowledge authors and be non-commercial, you don’t have to license your derivative works "
			                 "on the same terms.") },
			{ "BY-NC-SA", q_("You may remix, adapt, and build upon this sky culture non-commercially, as long as you credit "
			                 "authors and license your new creations under the identical terms.") },
			{ "BY-NC-ND", q_("You may use this sky culture and share them with others as long as you credit authors, but you can’t "
			                 "change it in any way or use it commercially.") },
		};

		color = "#33ff33"; // "green" area; free license
		if (license.contains("ND", Qt::CaseSensitive))
			color = "#ffff00"; // "yellow" area; nonfree license - weak restrictions
		if (license.contains("NC", Qt::CaseSensitive))
			color = "#ff6633"; // "red" area; nonfree license - strong restrictions

		if (!details.at(0).startsWith("CC0", Qt::CaseInsensitive)) // Not public domain!
			description.append(QString(" %1").arg(options.value(details.at(1), "")));
		else
			description = q_("This sky culture is distributed as public domain.");
	}
	else if (license.contains("FAL", Qt::CaseSensitive) || license.contains("Free Art License", Qt::CaseSensitive))
	{
		color = "#33ff33"; // "green" area; free license
		description.append(QString(" %1").arg(q_("Free Art License grants the right to freely copy, distribute, and transform.")));
	}

	return std::make_pair(color, description);
}

QString StelSkyCultureMgr::getCurrentSkyCultureHtmlLicense() const
{
	const auto lines = currentSkyCulture.license.split(QRegularExpression("\\s*\n+\\s*"), SkipEmptyParts);
	if (lines.isEmpty()) return "";

	switch (lines.size())
	{
	case 0:
	case 1:
	{
		const auto line = lines.isEmpty() ? "" : lines[0];
		const auto parts = line.split(":", SkipEmptyParts);
		const auto licenseName = convertReferenceLinks(parts.size() == 1 ? parts[0] : parts[1]);
		const auto [color, description] = getLicenseDescription(licenseName, true);
		if (!description.isEmpty())
		{
			return QString("<dl><dt><span style='color:%4;'>%5</span> <strong>%1: %2</strong></dt><dd><em>%3</em></dd></dl>")
				.arg(q_("License"),
				     currentSkyCulture.license.isEmpty() ? q_("unknown") : licenseName,
				     description, color, QChar(0x25CF));
		}
		else
		{
			return QString("<dl><dt><span style='color:%3;'>%4</span> <strong>%1: %2</strong></dt></dl>")
				.arg(q_("License"),
				     currentSkyCulture.license.isEmpty() ? q_("unknown") : licenseName, color, QChar(0x25CF));
		}
		return QString{};
	}
	default:
	{
		QString html = "<h1>" + q_("License") + "</h1>\n";
		QString addendum;
		for (const auto& line : lines)
		{
			const auto parts = line.split(QRegularExpression("\\s*:\\s*"), SkipEmptyParts);
			if (parts.size() == 1)
			{
				addendum += line + "<br>\n";
				continue;
			}
			const auto [color, description] = getLicenseDescription(parts[1], false);
			if (description.isEmpty())
			{
				html += QString("<dl><dt><span style='color:%2;'>%3</span> <strong>%1</strong></dt></dl>")
				               .arg(convertReferenceLinks(line), color, QChar(0x25CF));
			}
			else
			{
				html += QString("<dl><dt><span style='color:%3;'>%4</span> <strong>%1</strong></dt><dd><em>%2</em></dd></dl>")
				               .arg(convertReferenceLinks(line), description, color, QChar(0x25CF));
			}
		}
		return html + addendum;
	}
	}
}

QString StelSkyCultureMgr::getCurrentSkyCultureHtmlRegion() const
{
	QString html = "", region = currentSkyCulture.region.trimmed();
	QString description = q_("The region indicates the geographical area of origin of a given sky culture.");

	// special case: modern sky culture
	if (getCurrentSkyCultureID().contains("modern", Qt::CaseInsensitive))
	{
		// TRANSLATIONS: This is the name of a geographical "pseudo-region" on Earth
		region = N_("World");
		description = q_("All 'modern' sky cultures are based on the IAU-approved 88 constellations with standardized boundaries and are used worldwide. The origins of these constellations are pan-European.");
	}

	if (!region.isEmpty()) // Region marker is always 'green'
		html = QString("<dl><dt><span style='color:#33ff33;'>%4</span> <strong>%1 %2</strong></dt><dd><em>%3</em></dd></dl>").arg(q_("Region:"), q_(region), description, QChar(0x25CF));

	return html;
}

bool StelSkyCultureMgr::setCurrentSkyCultureNameI18(const QString& cultureName)
{
	return setCurrentSkyCultureID(skyCultureI18ToDirectory(cultureName));
}

//! returns newline delimited list of human readable culture names in english
QString StelSkyCultureMgr::getSkyCultureListEnglish(void) const
{
	QString cultures;
	QMapIterator<QString, StelSkyCulture> i(dirToNameEnglish);
	while(i.hasNext())
	{
		i.next();
		cultures += QString("%1\n").arg(i.value().englishName);
	}
	return cultures;
}

//! returns newline delimited list of human readable culture names translated to current locale
QStringList StelSkyCultureMgr::getSkyCultureListI18(void) const
{
	QStringList cultures;
	QMapIterator<QString, StelSkyCulture> i(dirToNameEnglish);
	while (i.hasNext())
	{
		i.next();
		cultures += qc_(i.value().englishName, "sky culture");
	}
	// Sort for GUI use. Note that e.g. German Umlauts are sorted after Z. TODO: Fix this!
	cultures.sort(Qt::CaseInsensitive);
	return cultures;
}

QStringList StelSkyCultureMgr::getSkyCultureListIDs(void) const
{
	return dirToNameEnglish.keys();
}

QString StelSkyCultureMgr::convertMarkdownLevel2Section(const QString& markdown, const QString& sectionName,
                                                  const qsizetype bodyStartPos, const qsizetype bodyEndPos)
{
	auto text = markdown.mid(bodyStartPos, bodyEndPos - bodyStartPos);

	if (sectionName.trimmed() == "References")
	{
		text.replace(QRegularExpression("^ - \\[#([0-9]+)\\]: (.*)$", QRegularExpression::MultilineOption),
		             "\\1. <span id=\"cite_\\1\">\\2</span>");
	}
	else
	{
		text = convertReferenceLinks(text);
	}

	if (sectionName.trimmed() == "License")
	{
		currentSkyCulture.license = text;
		return "";
	}
	if (sectionName.trimmed() == "Constellations")
	{
		// TODO: read out
		return "";
	}

	return markdownToHTML(text);
}

QString StelSkyCultureMgr::descriptionMarkdownToHTML(const QString& markdown, const QString& descrPath)
{
	const QRegularExpression headerPat("^# +(.+)$", QRegularExpression::MultilineOption);
	const auto match = headerPat.match(markdown);
	QString name;
	if (match.isValid())
	{
		name = match.captured(1);
	}
	else
	{
		qCritical().nospace() << "Failed to get sky culture name in file " << descrPath
		                      << ": got " << match.lastCapturedIndex() << " matches instead of 1";
		name = "Unknown";
	}

	QString text = 1+R"(
<style>
table, th, td {
  border: 1px solid black;
  border-collapse: collapse;
}
</style>
)";
	const QRegularExpression sectionNamePat("^## +(.+)$", QRegularExpression::MultilineOption);
	QString prevSectionName;
	qsizetype prevBodyStartPos = -1;
	for (auto it = sectionNamePat.globalMatch(markdown); it.hasNext(); )
	{
		const auto match = it.next();
		const auto sectionName = match.captured(1);
		const auto nameStartPos = match.capturedStart(0);
		const auto bodyStartPos = match.capturedEnd(0);
		if (!prevSectionName.isEmpty())
		{
			const auto sectionText = convertMarkdownLevel2Section(markdown, prevSectionName, prevBodyStartPos, nameStartPos);
			if (!sectionText.isEmpty())
			{
				text += "<h1>" + prevSectionName + "</h1>";
				text += sectionText;
			}
		}
		prevBodyStartPos = bodyStartPos;
		prevSectionName = sectionName;
	}
	if (prevBodyStartPos >= 0)
	{
		const auto sectionText = convertMarkdownLevel2Section(markdown, prevSectionName, prevBodyStartPos, markdown.size());
		if (!sectionText.isEmpty())
		{
			text += "<h1>" + prevSectionName + "</h1>\n";
			text += sectionText;
		}
	}

	return text;
}

QString StelSkyCultureMgr::getCurrentSkyCultureHtmlDescription()
{
	QString skyCultureId = getCurrentSkyCultureID();
	QString lang = StelApp::getInstance().getLocaleMgr().getAppLanguage();
	if (!QString("pt_BR zh_CN zh_HK zh_TW").contains(lang))
	{
		lang = lang.split("_").at(0);
	}
	const QString descPath = StelFileMgr::findFile("skycultures/" + skyCultureId + "/description.md");
	if (descPath.isEmpty())
		qWarning() << "WARNING: can't find description for skyculture" << skyCultureId;

	QString description;
	if (descPath.isEmpty())
	{
		description = QString("<h2>%1</2><p>%2</p>").arg(getCurrentSkyCultureNameI18(), q_("No description"));
	}
	else
	{
		QFile f(descPath);
		if(f.open(QIODevice::ReadOnly))
		{
			const auto markdown = QString::fromUtf8(f.readAll());
			description = descriptionMarkdownToHTML(markdown, descPath);
		}
		else
		{
			qWarning().nospace() << "Failed to open sky culture description file " << descPath << ": " << f.errorString();
		}
	}

	description.append(getCurrentSkyCultureHtmlClassification());
	description.append(getCurrentSkyCultureHtmlRegion());
	description.append(getCurrentSkyCultureHtmlLicense());


	return description;
}

QString StelSkyCultureMgr::directoryToSkyCultureEnglish(const QString& directory) const
{
	return dirToNameEnglish[directory].englishName;
}

QString StelSkyCultureMgr::directoryToSkyCultureI18(const QString& directory) const
{
	QString culture = dirToNameEnglish[directory].englishName;
	if (culture=="")
	{
		qWarning() << "WARNING: StelSkyCultureMgr::directoryToSkyCultureI18(\""
			   << QDir::toNativeSeparators(directory) << "\"): could not find directory";
		return "";
	}
	return q_(culture);
}

QString StelSkyCultureMgr::skyCultureI18ToDirectory(const QString& cultureName) const
{
	QMapIterator<QString, StelSkyCulture> i(dirToNameEnglish);
	while (i.hasNext())
	{
		i.next();
		if (qc_(i.value().englishName, "sky culture") == cultureName)
			return i.key();
	}
	return "";
}
