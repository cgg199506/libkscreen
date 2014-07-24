/*************************************************************************************
 *  Copyright (C) 2012 by Alejandro Fiestas Olivares <afiestas@kde.org>              *
 *  Copyright     2012 by Sebastian K�gler <sebas@kde.org>                           *
 *                                                                                   *
 *  This library is free software; you can redistribute it and/or                    *
 *  modify it under the terms of the GNU Lesser General Public                       *
 *  License as published by the Free Software Foundation; either                     *
 *  version 2.1 of the License, or (at your option) any later version.               *
 *                                                                                   *
 *  This library is distributed in the hope that it will be useful,                  *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of                   *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU                *
 *  Lesser General Public License for more details.                                  *
 *                                                                                   *
 *  You should have received a copy of the GNU Lesser General Public                 *
 *  License along with this library; if not, write to the Free Software              *
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA       *
 *************************************************************************************/

#define QT_GUI_LIB

#include <QtTest/QtTest>
#include <QtCore/QObject>

#include "../src/config.h"
#include "../src/output.h"
#include "../src/mode.h"

Q_LOGGING_CATEGORY(KSCREEN_QSCREEN, "kscreen.qscreen");

using namespace KScreen;

class testQScreenBackend : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void verifyOutputs();

private:
    QProcess m_process;
};

void testQScreenBackend::initTestCase()
{
    setenv("KSCREEN_BACKEND", "qscreen", 1);
//     setenv("KSCREEN_BACKEND", "xrandr", 1);
}

void testQScreenBackend::verifyOutputs()
{
    Config *config = Config::current();
    if (!config) {
        QSKIP("QScreenbackend invalid", SkipAll);
    }

    //QCOMPARE(QGuiApplication::screens().count(), config->outputs().count());


    bool primaryFound = false;
    foreach (const KScreen::Output* op, config->outputs()) {
        if (op->isPrimary()) {
            primaryFound = true;
        }
    }
    qDebug() << "Primary found? " << primaryFound;
    QVERIFY(primaryFound);
    QVERIFY(config->screen()->maxActiveOutputsCount() > 0);

    KScreen::Output *primary = config->primaryOutput();
    qDebug() << "ppp" << primary;
    QVERIFY(primary->isEnabled());
    //qDebug() << "Primary geometry? " << primary->geometry();
    qDebug() << " prim modes: " << primary->modes();
    //QVERIFY(primary->geometry() != QRectF(1,1,1,1));
//     Output *output = config->outputs().take(327);
//
//     QCOMPARE(output->name(), QString("default"));
//     QCOMPARE(output->type(), Output::Unknown);
//     QCOMPARE(output->modes().count(), 15);
//     QCOMPARE(output->pos(), QPoint(0, 0));
//     QCOMPARE(output->currentModeId(), QLatin1String("338"));
//     QCOMPARE(output->rotation(), Output::None);
//     QCOMPARE(output->isConnected(), true);
//     QCOMPARE(output->isEnabled(), true);
//     QCOMPARE(output->isPrimary(), false);
//     QVERIFY2(output->clones().isEmpty(), "In singleOutput is impossible to have clones");
}

QTEST_MAIN(testQScreenBackend)

#include "testqscreenbackend.moc"