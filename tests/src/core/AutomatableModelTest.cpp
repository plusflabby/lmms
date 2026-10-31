/*
 * AutomatableModelTest.cpp
 *
 * Copyright (c) 2019-2020 Johannes Lorenz <j.git$$$lorenz-ho.me, $$$=@>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */


#include <QtTest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include "AutomatableModel.h"
#include "ComboBoxModel.h"
#include "Engine.h"

class AutomatableModelTest : public QObject
{
	Q_OBJECT
public:
	bool m1Changed, m2Changed;
	void resetChanged() { m1Changed = m2Changed = false; }

private slots: // helper slots
	void onM1Changed() { m1Changed = true; }
	void onM2Changed() { m2Changed = true; }

private slots: // tests
	//! Test that upcast and exact casts work,
	//! but no downcast or any other casts

	void initTestCase()
	{
		using namespace lmms;
		Engine::init(true);
	}

	void cleanupTestCase()
	{
		using namespace lmms;
		Engine::destroy();
	}

	void CastTests()
	{
		using namespace lmms;

		ComboBoxModel comboModel;
		AutomatableModel* amPtr = &comboModel;
		QVERIFY(nullptr == amPtr->dynamicCast<FloatModel>()); // not a parent class
		QCOMPARE(&comboModel, amPtr->dynamicCast<AutomatableModel>()); // parent class
		QCOMPARE(&comboModel, amPtr->dynamicCast<IntModel>()); // parent class
		QCOMPARE(&comboModel, amPtr->dynamicCast<ComboBoxModel>()); // same class

		IntModel intModel;
		IntModel* imPtr = &intModel;
		QVERIFY(nullptr == imPtr->dynamicCast<FloatModel>()); // not a parent class
		QCOMPARE(&intModel, imPtr->dynamicCast<AutomatableModel>()); // parent class
		QCOMPARE(&intModel, imPtr->dynamicCast<IntModel>()); // same class
		QVERIFY(nullptr == imPtr->dynamicCast<ComboBoxModel>()); // child class
	}

	void LinkTests()
	{
		using namespace lmms;

		BoolModel m1(true), m2(false);

		QObject::connect(&m1, SIGNAL(dataChanged()),
			this, SLOT(onM1Changed()));
		QObject::connect(&m2, SIGNAL(dataChanged()),
			this, SLOT(onM2Changed()));

		resetChanged();
		m1.linkToModel(&m1);
		QVERIFY(!m1Changed); // cannot link to itself
		QVERIFY(!m2Changed);
		QVERIFY(m1.countLinks() == 0);

		resetChanged();
		m1.linkToModel(&m2);
		QVERIFY(m1.value() == m2.value()); // since m1 takes the value of m2
		QVERIFY(!m2Changed); // the second model is the source
		QVERIFY(m1.countLinks() == 1);

		resetChanged();
		m1.linkToModel(&m2);
		QVERIFY(!m1Changed); // it's already linked
		QVERIFY(!m2Changed);

		resetChanged();
		BoolModel m3(false);
		m1.setValue(1.f);
		m2.setValue(1.f);
		m1.linkToModel(&m2);
		QVERIFY(m1.value());
		QVERIFY(m2.value());
		QVERIFY(!m3.value());
		m2.linkToModel(&m3); // drag m3, drop on m2
		// m2 should take m3's (0) value
		QVERIFY(m2.value() == m3.value());
		QVERIFY(!m3.value());
		QVERIFY(m1.countLinks() == 2);
	}

	void LinkedModelUnlinkDuringNotification()
	{
		if (qEnvironmentVariableIsSet("LMMS_LINKED_MODEL_UNLINK_CHILD"))
		{
			using namespace lmms;
			BoolModel first(false), unlinked(false), last(false);
			first.linkToModel(&unlinked);
			unlinked.linkToModel(&last);
			connect(&last, &BoolModel::dataChanged, this, [&] { unlinked.unlink(); }, Qt::DirectConnection);

			first.setValue(1.0f);
			QVERIFY(first.value());
			QVERIFY(last.value());
			QVERIFY(!unlinked.value());
			return;
		}

		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		QProcess child;
		auto environment = QProcessEnvironment::systemEnvironment();
		environment.insert("LMMS_LINKED_MODEL_UNLINK_CHILD", "1");
		child.setProcessEnvironment(environment);
		child.start(QCoreApplication::applicationFilePath(),
			{"LinkedModelUnlinkDuringNotification", "-o", directory.filePath("child.log") + ",txt"});
		if (!child.waitForFinished(3000))
		{
			child.kill();
			child.waitForFinished();
			QFAIL("Linked-model propagation did not finish after a direct observer unlinked the next model");
		}
		QCOMPARE(child.exitStatus(), QProcess::NormalExit);
		QCOMPARE(child.exitCode(), 0);
	}
};

QTEST_GUILESS_MAIN(AutomatableModelTest)
#include "AutomatableModelTest.moc"
