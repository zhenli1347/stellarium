#pragma once

#include <tuple>
#include <vector>
#include <QHash>
#include <QString>

class DescriptionOldLoader
{
	QString markdown;
	QString inputDir;
	std::vector<QString> imageHRefs;
	struct DictEntry
	{
		QString comment;
		QString english;
		QString translated;
		bool operator<(const DictEntry& other) const
		{
			return std::tie(comment,english,translated) < std::tie(other.comment,other.english,other.translated);
		}
	};
	using TranslationDict = std::vector<DictEntry>;
	QHash<QString/*locale*/, TranslationDict> translations;
	QHash<QString/*locale*/, QString/*header*/> poHeaders;
	bool dumpMarkdown(const QString& outDir) const;
	void locateAllInlineImages(const QString& html);
	void loadTranslationsOfNames(const QString& poDir, const QString& cultureId);
public:
	void load(const QString& inDir, const QString& poDir, const QString& cultureId,
	          const QString& author, const QString& credit, const QString& license);
	bool dump(const QString& outDir) const;
};
