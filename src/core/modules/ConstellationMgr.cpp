/*
 * Stellarium
 * Copyright (C) 2002 Fabien Chereau
 * Copyright (C) 2012 Timothy Reaves
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


#include "ConstellationMgr.hpp"
#include "Constellation.hpp"
#include "StarMgr.hpp"
#include "StelUtils.hpp"
#include "StelApp.hpp"
#include "StelTextureMgr.hpp"
#include "StelProjector.hpp"
#include "StelObjectMgr.hpp"
#include "StelLocaleMgr.hpp"
#include "StelSkyCultureMgr.hpp"
#include "StelModuleMgr.hpp"
#include "StelMovementMgr.hpp"
#include "StelFileMgr.hpp"
#include "StelCore.hpp"
#include "StelPainter.hpp"
#include "Planet.hpp"
#include "StelUtils.hpp"

#include <vector>
#include <QDebug>
#include <QFile>
#include <QSettings>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QDir>

// constructor which loads all data from appropriate files
ConstellationMgr::ConstellationMgr(StarMgr *_hip_stars)
	: hipStarMgr(_hip_stars),
	  isolateSelected(false),
	  constellationPickEnabled(false),
	  constellationDisplayStyle(ConstellationMgr::constellationsTranslated),
	  artFadeDuration(2.),
	  artIntensity(0),
	  artIntensityMinimumFov(1.0),
	  artIntensityMaximumFov(2.0),
	  artDisplayed(0),
	  boundariesDisplayed(0),
	  linesDisplayed(0),
	  namesDisplayed(0),
	  checkLoadingData(false),
	  constellationLineThickness(1),
	  constellationBoundariesThickness(1)
{
	setObjectName("ConstellationMgr");
	Q_ASSERT(hipStarMgr);
}

ConstellationMgr::~ConstellationMgr()
{
	for (auto* constellation : constellations)
	{
		delete constellation;
	}

	for (auto* segment : allBoundarySegments)
	{
		delete segment;
	}
}

void ConstellationMgr::init()
{
	QSettings* conf = StelApp::getInstance().getSettings();
	Q_ASSERT(conf);

	asterFont.setPixelSize(conf->value("viewing/constellation_font_size", 15).toInt());
	setFlagLines(conf->value("viewing/flag_constellation_drawing").toBool());
	setFlagLabels(conf->value("viewing/flag_constellation_name").toBool());
	setFlagBoundaries(conf->value("viewing/flag_constellation_boundaries",false).toBool());	
	setArtIntensity(conf->value("viewing/constellation_art_intensity", 0.5f).toFloat());
	setArtFadeDuration(conf->value("viewing/constellation_art_fade_duration",2.f).toFloat());
	setFlagArt(conf->value("viewing/flag_constellation_art").toBool());
	setFlagIsolateSelected(conf->value("viewing/flag_constellation_isolate_selected", false).toBool());
	setFlagConstellationPick(conf->value("viewing/flag_constellation_pick", false).toBool());
	setConstellationLineThickness(conf->value("viewing/constellation_line_thickness", 1).toInt());
	setConstellationBoundariesThickness(conf->value("viewing/constellation_boundaries_thickness", 1).toInt());
	// The setting for developers
	setFlagCheckLoadingData(conf->value("devel/check_loading_constellation_data","false").toBool());

	QString starloreDisplayStyle=conf->value("viewing/constellation_name_style", "translated").toString();
	static const QMap<QString, ConstellationDisplayStyle>map={
		{ "translated",  constellationsTranslated},
		{ "native",      constellationsNative},
		{ "abbreviated", constellationsAbbreviated},
		{ "english",     constellationsEnglish}};
	if (!map.contains(starloreDisplayStyle))
	{
		qDebug() << "Warning: viewing/constellation_name_style (" << starloreDisplayStyle << ") invalid. Using translated style.";
		conf->setValue("viewing/constellation_name_style", "translated");
	}
	setConstellationDisplayStyle(map.value(starloreDisplayStyle, constellationsTranslated));

	// Load colors from config file
	QString defaultColor = conf->value("color/default_color").toString();
	setLinesColor(Vec3f(conf->value("color/const_lines_color", defaultColor).toString()));
	setBoundariesColor(Vec3f(conf->value("color/const_boundary_color", "0.8,0.3,0.3").toString()));
	setLabelsColor(Vec3f(conf->value("color/const_names_color", defaultColor).toString()));

	StelObjectMgr *objectManager = GETSTELMODULE(StelObjectMgr);
	objectManager->registerStelObjectMgr(this);
	connect(objectManager, SIGNAL(selectedObjectChanged(StelModule::StelModuleSelectAction)),
			this, SLOT(selectedObjectChange(StelModule::StelModuleSelectAction)));
	StelApp *app = &StelApp::getInstance();
	connect(app, SIGNAL(languageChanged()), this, SLOT(updateI18n()));
	connect(&app->getSkyCultureMgr(), &StelSkyCultureMgr::currentSkyCultureChanged, this, &ConstellationMgr::updateSkyCulture);

	QString displayGroup = N_("Display Options");
	addAction("actionShow_Constellation_Lines", displayGroup, N_("Constellation lines"), "linesDisplayed", "C");
	addAction("actionShow_Constellation_Art", displayGroup, N_("Constellation art"), "artDisplayed", "R");
	addAction("actionShow_Constellation_Labels", displayGroup, N_("Constellation labels"), "namesDisplayed", "V");
	addAction("actionShow_Constellation_Boundaries", displayGroup, N_("Constellation boundaries"), "boundariesDisplayed", "B");
	addAction("actionShow_Constellation_Isolated", displayGroup, N_("Select single constellation"), "isolateSelected"); // no shortcut, sync with GUI
	addAction("actionShow_Constellation_Deselect", displayGroup, N_("Remove selection of constellations"), this, "deselectConstellations()", "W");
	addAction("actionShow_Constellation_Select", displayGroup, N_("Select all constellations"), this, "selectAllConstellations()", "Alt+W");
	// Reload the current sky culture
	addAction("actionShow_SkyCulture_Reload", displayGroup, N_("Reload the sky culture"), this, "reloadSkyCulture()", "Ctrl+Alt+I");
}

/*************************************************************************
 Reimplementation of the getCallOrder method
*************************************************************************/
double ConstellationMgr::getCallOrder(StelModuleActionName actionName) const
{
	if (actionName==StelModule::ActionDraw)
		return StelApp::getInstance().getModuleMgr().getModule("GridLinesMgr")->getCallOrder(actionName)+10;
	return 0;
}

void ConstellationMgr::reloadSkyCulture()
{
	StelApp::getInstance().getSkyCultureMgr().reloadSkyCulture();
}

void ConstellationMgr::updateSkyCulture(const StelSkyCulture& skyCulture)
{
	// first of all, remove constellations from the list of selected objects in StelObjectMgr, since we are going to delete them
	deselectConstellations();
	loadLinesNamesAndArt(skyCulture.constellations, skyCulture.dirName,
	                     skyCulture.langsUseNativeNames.contains("en"));

	constellationsEnglishNames.clear();
	for (const auto*const cons : constellations)
	{
		constellationsEnglishNames.push_back(cons->englishName);
	}

	// Translate constellation names for the new sky culture
	updateI18n();

	loadBoundaries(skyCulture.boundaries, skyCulture.boundariesEpoch);

	if (getFlagCheckLoadingData())
	{
		int i = 1;
		for (auto* constellation : constellations)
		{
			qWarning() << "[Constellation] #" << i << " abbr:" << constellation->abbreviation << " name:" << constellation->getEnglishName() << " segments:" << constellation->numberOfSegments;
			i++;
		}
	}
}

void ConstellationMgr::selectedObjectChange(StelModule::StelModuleSelectAction action)
{
	StelObjectMgr* omgr = GETSTELMODULE(StelObjectMgr);
	Q_ASSERT(omgr);
	const QList<StelObjectP> newSelected = omgr->getSelectedObject();
	if (newSelected.empty())
	{
		// Even if do not have anything selected, KEEP constellation selection intact
		// (allows viewing constellations without distraction from star pointer animation)
		// setSelected(Q_NULLPTR);
		return;
	}

	const QList<StelObjectP> newSelectedConst = omgr->getSelectedObject("Constellation");
	if (!newSelectedConst.empty())
	{
		// If removing this selection
		if(action == StelModule::RemoveFromSelection)
		{
			unsetSelectedConst(static_cast<Constellation *>(newSelectedConst[0].data()));
		}
		else
		{
			// Add constellation to selected list (do not select a star, just the constellation)
			setSelectedConst(static_cast<Constellation *>(newSelectedConst[0].data()));
		}
	}
	else
	{
		QList<StelObjectP> newSelectedObject;
		if (StelApp::getInstance().getSkyCultureMgr().getCurrentSkyCultureBoundariesType()==StelSkyCulture::BoundariesType::IAU)
			newSelectedObject = omgr->getSelectedObject();
		else
			newSelectedObject = omgr->getSelectedObject("Star");

		if (!newSelectedObject.empty())
		{
			setSelected(newSelectedObject[0].data());
		}
		else
		{
			setSelected(Q_NULLPTR);
		}
	}
}

void ConstellationMgr::deselectConstellations(void)
{
	StelObjectMgr* omgr = GETSTELMODULE(StelObjectMgr);
	Q_ASSERT(omgr);
	if (getFlagIsolateSelected())
	{
		// The list of selected constellations is empty, but...
		if (selected.size()==0)
		{
			// ...let's unselect all constellations for guarantee
			for (auto* constellation : constellations)
			{
				constellation->setFlagLines(false);
				constellation->setFlagLabels(false);
				constellation->setFlagArt(false);
				constellation->setFlagBoundaries(false);
			}
		}

		// If any constellation is selected at the moment, then let's do not touch to it!
		if (omgr->getWasSelected() && !selected.empty())
			selected.pop_back();

		// Let's hide all previously selected constellations
		for (auto* constellation : selected)
		{
			constellation->setFlagLines(false);
			constellation->setFlagLabels(false);
			constellation->setFlagArt(false);
			constellation->setFlagBoundaries(false);
		}		
	}
	else
	{
		const QList<StelObjectP> newSelectedConst = omgr->getSelectedObject("Constellation");
		if (!newSelectedConst.empty())
			omgr->unSelect();
	}
	selected.clear();
}

void ConstellationMgr::selectAllConstellations()
{
	for (auto* constellation : constellations)
	{
		setSelectedConst(constellation);
	}
}

void ConstellationMgr::selectConstellation(const QString &englishName)
{
	if (!getFlagIsolateSelected())
		setFlagIsolateSelected(true); // Enable isolated selection

	bool found = false;
	for (auto* constellation : constellations)
	{
		if (constellation->getEnglishName().toLower()==englishName.toLower())
		{
			setSelectedConst(constellation);
			found = true;
		}
	}
	if (!found)
		qDebug() << "The constellation" << englishName << "is not found";
}

void ConstellationMgr::selectConstellationByObjectName(const QString &englishName)
{
	if (!getFlagIsolateSelected())
		setFlagIsolateSelected(true); // Enable isolated selection

	if (StelApp::getInstance().getSkyCultureMgr().getCurrentSkyCultureBoundariesType()==StelSkyCulture::BoundariesType::IAU)
		setSelectedConst(isObjectIn(GETSTELMODULE(StelObjectMgr)->searchByName(englishName).data()));
	else
		setSelectedConst(isStarIn(GETSTELMODULE(StelObjectMgr)->searchByName(englishName).data()));
}

void ConstellationMgr::deselectConstellation(const QString &englishName)
{
	if (!getFlagIsolateSelected())
		setFlagIsolateSelected(true); // Enable isolated selection

	bool found = false;
	for (auto* constellation : constellations)
	{
		if (constellation->getEnglishName().toLower()==englishName.toLower())
		{
			unsetSelectedConst(constellation);
			found = true;
		}
	}

	if (selected.size()==0 && found)
	{
		// Let's remove the selection for all constellations if the list of selected constellations is empty
		for (auto* constellation : constellations)
		{
			constellation->setFlagLines(false);
			constellation->setFlagLabels(false);
			constellation->setFlagArt(false);
			constellation->setFlagBoundaries(false);
		}
	}

	if (!found)
		qDebug() << "The constellation" << englishName << "is not found";
}

void ConstellationMgr::setLinesColor(const Vec3f& color)
{
	if (color != Constellation::lineColor)
	{
		Constellation::lineColor = color;
		emit linesColorChanged(color);
	}
}

Vec3f ConstellationMgr::getLinesColor() const
{
	return Constellation::lineColor;
}

void ConstellationMgr::setBoundariesColor(const Vec3f& color)
{
	if (Constellation::boundaryColor != color)
	{
		Constellation::boundaryColor = color;
		emit boundariesColorChanged(color);
	}
}

Vec3f ConstellationMgr::getBoundariesColor() const
{
	return Constellation::boundaryColor;
}

void ConstellationMgr::setLabelsColor(const Vec3f& color)
{
	if (Constellation::labelColor != color)
	{
		Constellation::labelColor = color;
		emit namesColorChanged(color);
	}
}

Vec3f ConstellationMgr::getLabelsColor() const
{
	return Constellation::labelColor;
}

void ConstellationMgr::setFontSize(const float newFontSize)
{
	if (asterFont.pixelSize() - newFontSize != 0.0f)
	{
		asterFont.setPixelSize(static_cast<int>(newFontSize));
		emit fontSizeChanged(newFontSize);
	}
}

float ConstellationMgr::getFontSize() const
{
	return asterFont.pixelSize();
}

void ConstellationMgr::setConstellationDisplayStyle(ConstellationDisplayStyle style)
{
	constellationDisplayStyle=style;
	emit constellationsDisplayStyleChanged(constellationDisplayStyle);
}

QString ConstellationMgr::getConstellationDisplayStyleString(ConstellationDisplayStyle style)
{
	return (style == constellationsAbbreviated ? "abbreviated" : (style == constellationsNative ? "native" : "translated"));
}

ConstellationMgr::ConstellationDisplayStyle ConstellationMgr::getConstellationDisplayStyle()
{
	return constellationDisplayStyle;
}

void ConstellationMgr::setConstellationLineThickness(const int thickness)
{
	if(thickness!=constellationLineThickness)
	{
		constellationLineThickness = thickness;
		if (constellationLineThickness<=0) // The line can not be negative or zero thickness
			constellationLineThickness = 1;

		emit constellationLineThicknessChanged(thickness);
	}
}

void ConstellationMgr::setConstellationBoundariesThickness(const int thickness)
{
	if(thickness!=constellationBoundariesThickness)
	{
		constellationBoundariesThickness = thickness;
		if (constellationBoundariesThickness<=0) // The line can not be negative or zero thickness
			constellationBoundariesThickness = 1;

		emit constellationBoundariesThicknessChanged(thickness);
	}
}

void ConstellationMgr::loadLinesNamesAndArt(const QJsonArray &constellationsData, const QString &cultureName, const bool preferNativeNames)
{
	constellations.clear();

	int readOk = 0;
	for (const auto& constellationData : constellationsData)
	{
		Constellation*const cons = new Constellation;
		const auto consObj = constellationData.toObject();
		if (!cons->read(consObj, hipStarMgr, preferNativeNames))
		{
			delete cons;
			continue;
		}
		++readOk;
		constellations.push_back(cons);

		cons->artOpacity = artIntensity;
		cons->artFader.setDuration(static_cast<int>(artFadeDuration * 1000.f));
		cons->setFlagArt(artDisplayed);
		cons->setFlagBoundaries(boundariesDisplayed);
		cons->setFlagLines(linesDisplayed);
		cons->setFlagLabels(namesDisplayed);

		// Now load constellation art

		const auto imgVal = consObj["image"];
		if (imgVal.isUndefined())
			continue;
		const auto imgData = imgVal.toObject();
		const auto anchors = imgData["anchors"].toArray();
		if (anchors.size() < 3)
		{
			qWarning().nospace() << "Bad number of anchors (" << anchors.size() << ") for image in constellation "
			                     << consObj["id"].toString();
			continue;
		}
		const auto anchor1 = anchors[0].toObject();
		const auto anchor2 = anchors[1].toObject();
		const auto anchor3 = anchors[2].toObject();
		const auto xy1 = anchor1["pos"].toArray();
		const auto xy2 = anchor2["pos"].toArray();
		const auto xy3 = anchor3["pos"].toArray();

		const int x1 = xy1[0].toInt();
		const int y1 = xy1[1].toInt();
		const int x2 = xy2[0].toInt();
		const int y2 = xy2[1].toInt();
		const int x3 = xy3[0].toInt();
		const int y3 = xy3[1].toInt();
		const int hp1 = anchor1["hip"].toInt();
		const int hp2 = anchor2["hip"].toInt();
		const int hp3 = anchor3["hip"].toInt();

		const auto texfile = imgData["file"].toString();

		const auto texturePath = StelFileMgr::findFile("skycultures/"+cultureName+"/"+texfile);
		if (texturePath.isEmpty())
		{
			qWarning() << "ERROR: could not find texture" << QDir::toNativeSeparators(texfile);
		}

		cons->artTexture = StelApp::getInstance().getTextureManager().createTextureThread(texturePath, StelTexture::StelTextureParams(true));

		const auto sizeData = imgData["size"].toArray();
		if (sizeData.size() != 2)
		{
			qWarning().nospace() << "Bad length of \"size\" array for image in constellation "
			                     << consObj["id"].toString();
			continue;
		}
		const int texSizeX = sizeData[0].toInt(), texSizeY = sizeData[1].toInt();

		StelCore* core = StelApp::getInstance().getCore();
		const Vec3d s1 = hipStarMgr->searchHP(static_cast<int>(hp1))->getJ2000EquatorialPos(core);
		const Vec3d s2 = hipStarMgr->searchHP(static_cast<int>(hp2))->getJ2000EquatorialPos(core);
		const Vec3d s3 = hipStarMgr->searchHP(static_cast<int>(hp3))->getJ2000EquatorialPos(core);

		// To transform from texture coordinate to 2d coordinate we need to find X with XA = B
		// A formed of 4 points in texture coordinate, B formed with 4 points in 3d coordinate space
		// We need 3 stars and the 4th point is deduced from the others to get a normal base
		// X = B inv(A)
		Vec3d s4 = s1 + ((s2 - s1) ^ (s3 - s1));
		Mat4d B(s1[0], s1[1], s1[2], 1, s2[0], s2[1], s2[2], 1, s3[0], s3[1], s3[2], 1, s4[0], s4[1], s4[2], 1);
		Mat4d A(x1, texSizeY - static_cast<int>(y1), 0., 1., x2, texSizeY - static_cast<int>(y2), 0., 1., x3, texSizeY - static_cast<int>(y3), 0., 1., x1, texSizeY - static_cast<int>(y1), texSizeX, 1.);
		Mat4d X = B * A.inverse();

		// Tessellate on the plane assuming a tangential projection for the image
		static const int nbPoints=5;
		QVector<Vec2f> texCoords;
		texCoords.reserve(nbPoints*nbPoints*6);
		for (int j=0;j<nbPoints;++j)
		{
			for (int i=0;i<nbPoints;++i)
			{
				texCoords << Vec2f((static_cast<float>(i))/nbPoints, (static_cast<float>(j))/nbPoints);
				texCoords << Vec2f((static_cast<float>(i)+1.f)/nbPoints, (static_cast<float>(j))/nbPoints);
				texCoords << Vec2f((static_cast<float>(i))/nbPoints, (static_cast<float>(j)+1.f)/nbPoints);
				texCoords << Vec2f((static_cast<float>(i)+1.f)/nbPoints, (static_cast<float>(j))/nbPoints);
				texCoords << Vec2f((static_cast<float>(i)+1.f)/nbPoints, (static_cast<float>(j)+1.f)/nbPoints);
				texCoords << Vec2f((static_cast<float>(i))/nbPoints, (static_cast<float>(j)+1.f)/nbPoints);
			}
		}

		QVector<Vec3d> contour;
		contour.reserve(texCoords.size());
		for (const auto& v : qAsConst(texCoords))
		{
			Vec3d vertex = X * Vec3d(static_cast<double>(v[0]) * texSizeX, static_cast<double>(v[1]) * texSizeY, 0.);
			// Originally the projected texture plane remained as tangential plane.
			// The vertices should however be reduced to the sphere for correct aberration:
			vertex.normalize();
			contour << vertex;
		}

		cons->artPolygon.vertex=contour;
		cons->artPolygon.texCoords=texCoords;
		cons->artPolygon.primitiveType=StelVertexArray::Triangles;

		Vec3d tmp(X * Vec3d(0.5*texSizeX, 0.5*texSizeY, 0.));
		tmp.normalize();
		Vec3d tmp2(X * Vec3d(0., 0., 0.));
		tmp2.normalize();
		cons->boundingCap.n=tmp;
		cons->boundingCap.d=tmp*tmp2;
	}

	qDebug() << "Loaded" << readOk << "/" << constellations.size() << "constellation records successfully for culture" << cultureName;

	// Set current states
	setFlagArt(artDisplayed);
	setFlagLines(linesDisplayed);
	setFlagLabels(namesDisplayed);
	setFlagBoundaries(boundariesDisplayed);

}

void ConstellationMgr::draw(StelCore* core)
{
	const StelProjectorP prj = core->getProjection(StelCore::FrameJ2000);
	StelPainter sPainter(prj);
	sPainter.setFont(asterFont);
	drawLines(sPainter, core);
	Vec3d vel(0.);
	if (core->getUseAberration())
	{
		vel=core->getCurrentPlanet()->getHeliocentricEclipticVelocity();
		vel=StelCore::matVsop87ToJ2000*vel;
		vel*=core->getAberrationFactor() * (AU/(86400.0*SPEED_OF_LIGHT));
	}
	drawNames(sPainter, vel);
	drawArt(sPainter, vel);
	drawBoundaries(sPainter, vel);
}

// Draw constellations art textures
void ConstellationMgr::drawArt(StelPainter& sPainter, const Vec3d &obsVelocity) const
{
	sPainter.setBlending(true, GL_ONE, GL_ONE);
	sPainter.setCullFace(true);

	SphericalRegionP region = sPainter.getProjector()->getViewportConvexPolygon();
	for (auto* constellation : constellations)
	{
		constellation->drawArtOptim(sPainter, *region, obsVelocity);
	}

	sPainter.setCullFace(false);
}

// Draw constellations lines
void ConstellationMgr::drawLines(StelPainter& sPainter, const StelCore* core) const
{
	const float ppx = static_cast<float>(sPainter.getProjector()->getDevicePixelsPerPixel());
	sPainter.setBlending(true);
	if (constellationLineThickness>1 || ppx>1.f)
		sPainter.setLineWidth(constellationLineThickness*ppx); // set line thickness
	sPainter.setLineSmooth(true);

	const SphericalCap& viewportHalfspace = sPainter.getProjector()->getBoundingCap();
	for (auto* constellation : constellations)
	{
		constellation->drawOptim(sPainter, core, viewportHalfspace);
	}
	if (constellationLineThickness>1 || ppx>1.f)
		sPainter.setLineWidth(1); // restore line thickness
	sPainter.setLineSmooth(false);
}

// Draw the names of all the constellations
void ConstellationMgr::drawNames(StelPainter& sPainter, const Vec3d &obsVelocity) const
{
	sPainter.setBlending(true);
	for (auto* constellation : constellations)
	{
		Vec3d XYZname=constellation->XYZname;
		XYZname.normalize();
		XYZname+=obsVelocity;
		XYZname.normalize();

		// Check if in the field of view
		if (sPainter.getProjector()->projectCheck(XYZname, constellation->XYname))
			constellation->drawName(sPainter, constellationDisplayStyle);
	}
}

Constellation *ConstellationMgr::isStarIn(const StelObject* s) const
{
	for (auto* constellation : constellations)
	{
		// Check if the star is in one of the constellation
		if (constellation->isStarIn(s))
		{
			return constellation;
		}
	}
	return Q_NULLPTR;
}

Constellation* ConstellationMgr::findFromAbbreviation(const QString& abbreviation) const
{
	// search in uppercase only
	//QString tname = abbreviation.toUpper();

	for (auto* constellation : constellations)
	{
		//if (constellation->abbreviation.toUpper() == tname)
		if (constellation->abbreviation.compare(abbreviation, Qt::CaseInsensitive) == 0)
		{
			//if (constellation->abbreviation != abbreviation)
			//	qDebug() << "ConstellationMgr::findFromAbbreviation: not a perfect match, but sufficient:" << constellation->abbreviation << "vs." << abbreviation;
			return constellation;
		}
		//else qDebug() << "Comparison mismatch: " << abbreviation << "vs." << constellation->abbreviation;
	}
	return Q_NULLPTR;
}

// Can't find constellation from a position because it's not well localized
QList<StelObjectP> ConstellationMgr::searchAround(const Vec3d&, double, const StelCore*) const
{
	return QList<StelObjectP>();
}

QStringList ConstellationMgr::getConstellationsEnglishNames()
{
	return  constellationsEnglishNames;
}

void ConstellationMgr::updateI18n()
{
	const StelTranslator& trans = StelApp::getInstance().getLocaleMgr().getSkyTranslator();

	for (auto* constellation : constellations)
	{
		constellation->nameI18 = trans.tryQtranslate(constellation->englishName, "constellation");
		if (constellation->nameI18.isEmpty())
			constellation->nameI18 = qc_(constellation->englishName, "constellation");
	}
}

// update faders
void ConstellationMgr::update(double deltaTime)
{
	//calculate FOV fade value, linear fade between artIntensityMaximumFov and artIntensityMinimumFov
	double fov = StelApp::getInstance().getCore()->getMovementMgr()->getCurrentFov();
	Constellation::artIntensityFovScale = static_cast<float>(qBound(0.0,(fov - artIntensityMinimumFov) / (artIntensityMaximumFov - artIntensityMinimumFov),1.0));

	const int delta = static_cast<int>(deltaTime*1000);
	for (auto* constellation : constellations)
	{
		constellation->update(delta);
	}
}

void ConstellationMgr::setArtIntensity(const float intensity)
{
	if ((artIntensity - intensity) != 0.0f)
	{
		artIntensity = intensity;

		for (auto* constellation : constellations)
		{
			constellation->artOpacity = artIntensity;
		}

		emit artIntensityChanged(static_cast<double>(intensity));
	}
}

float ConstellationMgr::getArtIntensity() const
{
	return artIntensity;
}

void ConstellationMgr::setArtIntensityMinimumFov(const double fov)
{
	artIntensityMinimumFov = fov;
}

double ConstellationMgr::getArtIntensityMinimumFov() const
{
	return artIntensityMinimumFov;
}

void ConstellationMgr::setArtIntensityMaximumFov(const double fov)
{
	artIntensityMaximumFov = fov;
}

double ConstellationMgr::getArtIntensityMaximumFov() const
{
	return artIntensityMaximumFov;
}

void ConstellationMgr::setArtFadeDuration(const float duration)
{
    if (!qFuzzyCompare(artFadeDuration, duration))
	{
		artFadeDuration = duration;

		for (auto* constellation : constellations)
		{
			constellation->artFader.setDuration(static_cast<int>(duration * 1000.f));
		}
		emit artFadeDurationChanged(duration);
	}
}

float ConstellationMgr::getArtFadeDuration() const
{
	return artFadeDuration;
}

void ConstellationMgr::setFlagLines(const bool displayed)
{
	if(linesDisplayed != displayed)
	{
		linesDisplayed = displayed;
		if (!selected.empty() && isolateSelected)
		{
			for (auto* constellation : selected)
			{
				constellation->setFlagLines(linesDisplayed);
			}
		}
		else
		{
			for (auto* constellation : constellations)
			{
				constellation->setFlagLines(linesDisplayed);
			}
		}
		emit linesDisplayedChanged(displayed);
	}
}

bool ConstellationMgr::getFlagLines(void) const
{
	return linesDisplayed;
}

void ConstellationMgr::setFlagBoundaries(const bool displayed)
{
	if (boundariesDisplayed != displayed)
	{
		boundariesDisplayed = displayed;
		if (!selected.empty() && isolateSelected)
		{
			for (auto* constellation : selected)
			{
				constellation->setFlagBoundaries(boundariesDisplayed);
			}
		}
		else
		{
			for (auto* constellation : constellations)
			{
				constellation->setFlagBoundaries(boundariesDisplayed);
			}
		}
		emit boundariesDisplayedChanged(displayed);
	}
}

bool ConstellationMgr::getFlagBoundaries(void) const
{
	return boundariesDisplayed;
}

void ConstellationMgr::setFlagArt(const bool displayed)
{
	if (artDisplayed != displayed)
	{
		artDisplayed = displayed;
		if (!selected.empty() && isolateSelected)
		{
			for (auto* constellation : selected)
			{
				constellation->setFlagArt(artDisplayed);
			}
		}
		else
		{
			for (auto* constellation : constellations)
			{
				constellation->setFlagArt(artDisplayed);
			}
		}
		emit artDisplayedChanged(displayed);
	}
}

bool ConstellationMgr::getFlagArt(void) const
{
	return artDisplayed;
}

void ConstellationMgr::setFlagLabels(const bool displayed)
{
	if (namesDisplayed != displayed)
	{
		namesDisplayed = displayed;
		if (!selected.empty() && isolateSelected)
		{
			for (auto* constellation : selected)
				constellation->setFlagLabels(namesDisplayed);
		}
		else
		{
			for (auto* constellation : constellations)
				constellation->setFlagLabels(namesDisplayed);
		}
		emit namesDisplayedChanged(displayed);
	}
}

bool ConstellationMgr::getFlagLabels(void) const
{
	return namesDisplayed;
}

void ConstellationMgr::setFlagIsolateSelected(const bool isolate)
{
	if (isolateSelected != isolate)
	{
		isolateSelected = isolate;

		// when turning off isolated selection mode, clear existing isolated selections.
		if (!isolateSelected)
		{
			for (auto* constellation : constellations)
			{
				constellation->setFlagLines(getFlagLines());
				constellation->setFlagLabels(getFlagLabels());
				constellation->setFlagArt(getFlagArt());
				constellation->setFlagBoundaries(getFlagBoundaries());
			}
		}
		emit isolateSelectedChanged(isolate);
	}
}

bool ConstellationMgr::getFlagIsolateSelected(void) const
{
	return isolateSelected;
}

void ConstellationMgr::setFlagConstellationPick(const bool mode)
{
	constellationPickEnabled = mode;
}

bool ConstellationMgr::getFlagConstellationPick(void) const
{
	return constellationPickEnabled;
}

StelObject* ConstellationMgr::getSelected(void) const
{
	return *selected.begin();  // TODO return all or just remove this method
}

void ConstellationMgr::setSelected(const QString& abbreviation)
{
	Constellation * c = findFromAbbreviation(abbreviation);
	if(c != Q_NULLPTR) setSelectedConst(c);
}

StelObjectP ConstellationMgr::setSelectedStar(const QString& abbreviation)
{
	Constellation * c = findFromAbbreviation(abbreviation);
	if(c != Q_NULLPTR)
	{
		setSelectedConst(c);
		return c->getBrightestStarInConstellation();
	}
	return Q_NULLPTR;
}

void ConstellationMgr::setSelectedConst(Constellation * c)
{
	// update states for other constellations to fade them out
	if (c != Q_NULLPTR)
	{
		selected.push_back(c);

		if (isolateSelected)
		{
			if (!getFlagConstellationPick())
			{
				// Propagate current settings to newly selected constellation
				c->setFlagLines(getFlagLines());
				c->setFlagLabels(getFlagLabels());
				c->setFlagArt(getFlagArt());
				c->setFlagBoundaries(getFlagBoundaries());

				for (auto* constellation : constellations)
				{
					bool match = false;
					for (auto* selected_constellation : selected)
					{
						if (constellation == selected_constellation)
						{
							match=true; // this is a selected constellation
							break;
						}
					}

					if(!match)
					{
						// Not selected constellation
						constellation->setFlagLines(false);
						constellation->setFlagLabels(false);
						constellation->setFlagArt(false);
						constellation->setFlagBoundaries(false);
					}
				}
			}
			else
			{
				for (auto* constellation : constellations)
				{
					constellation->setFlagLines(false);
					constellation->setFlagLabels(false);
					constellation->setFlagArt(false);
					constellation->setFlagBoundaries(false);
				}

				// Propagate current settings to newly selected constellation
				c->setFlagLines(getFlagLines());
				c->setFlagLabels(getFlagLabels());
				c->setFlagArt(getFlagArt());
				c->setFlagBoundaries(getFlagBoundaries());
			}

			Constellation::singleSelected = true;  // For boundaries
		}
		else
			Constellation::singleSelected = false; // For boundaries
	}
	else
	{
		if (selected.empty()) return;

		// Otherwise apply standard flags to all constellations
		for (auto* constellation : constellations)
		{
			constellation->setFlagLines(getFlagLines());
			constellation->setFlagLabels(getFlagLabels());
			constellation->setFlagArt(getFlagArt());
			constellation->setFlagBoundaries(getFlagBoundaries());
		}

		// And remove all selections
		selected.clear();
	}
}

//! Remove a constellation from the selected constellation list
void ConstellationMgr::unsetSelectedConst(Constellation * c)
{
	if (c != Q_NULLPTR)
	{
		for (auto iter = selected.begin(); iter != selected.end();)
		{
			if( (*iter)->getEnglishName() == c->getEnglishName() )
			{
				iter = selected.erase(iter);
			}
			else
			{
				++iter;
			}
		}

		// If no longer any selection, restore all flags on all constellations
		if (selected.empty())
		{
			// Otherwise apply standard flags to all constellations
			for (auto* constellation : constellations)
			{
				constellation->setFlagLines(getFlagLines());
				constellation->setFlagLabels(getFlagLabels());
				constellation->setFlagArt(getFlagArt());
				constellation->setFlagBoundaries(getFlagBoundaries());
			}

			Constellation::singleSelected = false; // For boundaries
		}
		else if(isolateSelected)
		{
			// No longer selected constellation
			c->setFlagLines(false);
			c->setFlagLabels(false);
			c->setFlagArt(false);
			c->setFlagBoundaries(false);

			Constellation::singleSelected = true;  // For boundaries
		}
	}
}

bool ConstellationMgr::loadBoundaries(const QJsonArray& boundaryData, const QString& boundariesEpoch)
{
	// delete existing boundaries if any exist
	for (auto* segment : allBoundarySegments)
	{
		delete segment;
	}
	allBoundarySegments.clear();

	qDebug() << "Loading constellation boundary data ... ";

	for (int n = 0; n < boundaryData.size(); ++n)
	{
		const auto line = boundaryData[n].toString().toStdString();
		char dec1_sign, dec2_sign;
		int ra1_h, ra1_m, ra1_s, dec1_d, dec1_m, dec1_s;
		int ra2_h, ra2_m, ra2_s, dec2_d, dec2_m, dec2_s;
		char constellationNames[2][8];
		if (sscanf(line.c_str(),
		           "%*s %*s"
		           "%d:%d:%d %c%d:%d:%d "
		           "%d:%d:%d %c%d:%d:%d "
		           "%7s %7s",
		           &ra1_h, &ra1_m, &ra1_s,
		           &dec1_sign, &dec1_d, &dec1_m, &dec1_s,
		           &ra2_h, &ra2_m, &ra2_s,
		           &dec2_sign, &dec2_d, &dec2_m, &dec2_s,
		           constellationNames[0], constellationNames[1]) != 16)
		{
			qWarning().nospace() << "Failed to parse skyculture boundary line: \"" << line.c_str() << "\"";
			continue;
		}

		constexpr double timeSecToRadians = M_PI / (12 * 3600);
		double RA1 = (60. * (60. * ra1_h + ra1_m) + ra1_s) * timeSecToRadians;
		double RA2 = (60. * (60. * ra2_h + ra2_m) + ra2_s) * timeSecToRadians;
		constexpr double angleSecToRad = M_PI / (180 * 3600);
		const double DE1 = (60. * (60. * dec1_d + dec1_m) + dec1_s) * (dec1_sign=='-' ? -1 : 1) * angleSecToRad;
		const double DE2 = (60. * (60. * dec2_d + dec2_m) + dec2_s) * (dec2_sign=='-' ? -1 : 1) * angleSecToRad;

		bool b1875 = false;
		if (boundariesEpoch.toUpper() == "B1875")
			b1875 = true;
		else if (boundariesEpoch.toUpper() != "J2000")
			qWarning() << "Unexpected epoch for boundaries:" << boundariesEpoch;

		const auto& core = *StelApp::getInstance().getCore();

		const int numPoints = 2 + std::ceil(std::abs(RA1 - RA2) / (M_PI / 64));
		Vec3d xyz1;
		StelUtils::spheToRect(RA1,DE1,xyz1);
		Vec3d xyz2;
		StelUtils::spheToRect(RA2,DE2,xyz2);

		const auto points = new std::vector<Vec3d>;

		// Make sure the interpolation works without problems when jumping over 2pi
		if (RA2 - RA1 > M_PI) RA2 -= 2 * M_PI;
		if (RA1 - RA2 > M_PI) RA1 -= 2 * M_PI;

		for (double n = 0; n < numPoints; ++n)
		{
			const double t = n / (numPoints - 1);
			const double RA = RA1 + t * (RA2 - RA1);
			const double DE = DE1 + t * (DE2 - DE1);
			Vec3d xyz;
			StelUtils::spheToRect(RA,DE,xyz);
			if (b1875) xyz = core.j1875ToJ2000(xyz);
			points->push_back(xyz);
		}

		Constellation *cons = Q_NULLPTR;
		for (QString consName : constellationNames)
		{
			// not used?
			if (consName == "SER1" || consName == "SER2") consName = "SER";

			cons = findFromAbbreviation(consName);
			if (!cons)
				qWarning() << "ERROR while processing boundary file - cannot find constellation:" << consName;
			else
				cons->isolatedBoundarySegments.push_back(points);
		}

		if (cons)
		{
			cons->sharedBoundarySegments.push_back(points);
			// this list is for the de-allocation
			allBoundarySegments.push_back(points);
		}
		else
		{
			delete points;
		}
	}
	qDebug() << "Loaded" << boundaryData.size() << "constellation boundary segments";

	return true;
}

void ConstellationMgr::drawBoundaries(StelPainter& sPainter, const Vec3d &obsVelocity) const
{
	const float ppx = static_cast<float>(sPainter.getProjector()->getDevicePixelsPerPixel());
	sPainter.setBlending(false);
	if (constellationBoundariesThickness>1 || ppx>1.f)
		sPainter.setLineWidth(constellationBoundariesThickness*ppx); // set line thickness
	sPainter.setLineSmooth(true);
	for (auto* constellation : constellations)
	{
		constellation->drawBoundaryOptim(sPainter, obsVelocity);
	}
	if (constellationBoundariesThickness>1 || ppx>1.f)
		sPainter.setLineWidth(1); // restore line thickness
	sPainter.setLineSmooth(false);
}

StelObjectP ConstellationMgr::searchByNameI18n(const QString& nameI18n) const
{
	QString objw = nameI18n.toUpper();

	for (auto* constellation : constellations)
	{
		QString objwcap = constellation->nameI18.toUpper();
		if (objwcap == objw) return constellation;
	}
	return Q_NULLPTR;
}

StelObjectP ConstellationMgr::searchByName(const QString& name) const
{
	QString objw = name.toUpper();
	for (auto* constellation : constellations)
	{
		QString objwcap = constellation->englishName.toUpper();
		if (objwcap == objw) return constellation;

		objwcap = constellation->abbreviation.toUpper();
		if (objwcap == objw) return constellation;
	}
	return Q_NULLPTR;
}

StelObjectP ConstellationMgr::searchByID(const QString &id) const
{
	for (auto* constellation : constellations)
	{
		if (constellation->getID() == id) return constellation;
	}
	return Q_NULLPTR;
}

QStringList ConstellationMgr::listAllObjects(bool inEnglish) const
{
	QStringList result;
	if (inEnglish)
	{
		for (auto* constellation : constellations)
		{
			result << constellation->getEnglishName();
		}
	}
	else
	{
		for (auto* constellation : constellations)
		{
			result << constellation->getNameI18n();
		}
	}
	return result;
}

QString ConstellationMgr::getStelObjectType() const
{
	return Constellation::CONSTELLATION_TYPE;
}

void ConstellationMgr::setSelected(const StelObject *s)
{
	if (!s)
		setSelectedConst(Q_NULLPTR);
	else
	{
		if (StelApp::getInstance().getSkyCultureMgr().getCurrentSkyCultureBoundariesType()==StelSkyCulture::BoundariesType::IAU)
			setSelectedConst(isObjectIn(s));
		else
			setSelectedConst(isStarIn(s));
	}
}

Constellation* ConstellationMgr::isObjectIn(const StelObject *s) const
{
	StelCore *core = StelApp::getInstance().getCore();
	QString IAUConst = core->getIAUConstellation(s->getEquinoxEquatorialPos(core));
	for (auto* constellation : constellations)
	{
		// Check if the object is in the constellation
		if (constellation->getShortName().toUpper() == IAUConst.toUpper())
			return constellation;
	}
	return Q_NULLPTR;
}
