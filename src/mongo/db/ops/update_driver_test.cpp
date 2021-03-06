/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/ops/update_driver.h"

#include "mongo/db/field_ref_set.h"
#include "mongo/db/index_set.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::BSONObj;
    using mongo::FieldRef;
    using mongo::FieldRefSet;
    using mongo::fromjson;
    using mongo::IndexPathSet;
    using mongo::mutablebson::Document;
    using mongo::StringData;
    using mongo::UpdateDriver;

    TEST(Parse, Normal) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{$set:{a:1}}")));
        ASSERT_EQUALS(driver.numMods(), 1U);
        ASSERT_FALSE(driver.isDocReplacement());
    }

    TEST(Parse, MultiMods) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{$set:{a:1, b:1}}")));
        ASSERT_EQUALS(driver.numMods(), 2U);
        ASSERT_FALSE(driver.isDocReplacement());
    }

    TEST(Parse, MixingMods) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{$set:{a:1}, $unset:{b:1}}")));
        ASSERT_EQUALS(driver.numMods(), 2U);
        ASSERT_FALSE(driver.isDocReplacement());
    }

    TEST(Parse, ObjectReplacment) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{obj: \"obj replacement\"}")));
        ASSERT_TRUE(driver.isDocReplacement());
    }

    TEST(Parse, EmptyMod) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_NOT_OK(driver.parse(fromjson("{$set:{}}")));
    }

    TEST(Parse, WrongMod) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_NOT_OK(driver.parse(fromjson("{$xyz:{a:1}}")));
    }

    TEST(Parse, WrongType) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_NOT_OK(driver.parse(fromjson("{$set:[{a:1}]}")));
    }

    TEST(Parse, ModsWithLaterObjReplacement)  {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_NOT_OK(driver.parse(fromjson("{$set:{a:1}, obj: \"obj replacement\"}")));
    }

    TEST(Parse, PushAll) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{$pushAll:{a:[1,2,3]}}")));
        ASSERT_EQUALS(driver.numMods(), 1U);
        ASSERT_FALSE(driver.isDocReplacement());
    }

    TEST(Parse, SetOnInsert) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{$setOnInsert:{a:1}}")));
        ASSERT_EQUALS(driver.numMods(), 1U);
        ASSERT_FALSE(driver.isDocReplacement());
    }

    // A base class for shard key immutability tests. We construct a document (see 'setUp'
    // below for the document structure), and declare the two subfields "s.a' and 's.b' to be
    // the shard keys, then test that various mutations that affect (or don't) the shard keys
    // are rejected (or permitted).
    class ShardKeyTest : public mongo::unittest::Test {
    public:
        ShardKeyTest()
            : _shardKeyPattern(fromjson("{ 's.a' : 1, 's.c' : 1 }")) {
        }

        void setUp() {
            // All elements here are arrays so that we can perform a no-op that won't be
            // detected as such by the update code, which would foil our testing. Instead, we
            // use $push with $slice.
            _obj.reset(new BSONObj(
                           fromjson("{ x : [1], s : { a : [1], b : [2], c : [ 3, 3, 3 ] } }")));
            _doc.reset(new Document(*_obj));
            _driver.reset(new UpdateDriver(UpdateDriver::Options()));
        }

    protected:
        BSONObj _shardKeyPattern;
        boost::scoped_ptr<BSONObj> _obj;
        boost::scoped_ptr<Document> _doc;
        boost::scoped_ptr<UpdateDriver> _driver;
    };

    TEST_F(ShardKeyTest, NoOpsDoNotAffectShardKeys) {
        BSONObj mod(fromjson("{ $set : { 's.a.0' : 1, 's.c.0' : 3 } }"));
        ASSERT_OK(_driver->parse(mod));
        _driver->refreshShardKeyPattern(_shardKeyPattern);
        ASSERT_OK(_driver->update(StringData(), _doc.get(), NULL));
        ASSERT_FALSE(_driver->modsAffectShardKeys());
    }

    TEST_F(ShardKeyTest, MutatingShardKeyFieldRejected) {
        BSONObj mod(fromjson("{ $push : { 's.a' : { $each : [2], $slice : -1 } } }"));
        ASSERT_OK(_driver->parse(mod));
        _driver->refreshShardKeyPattern(_shardKeyPattern);
        ASSERT_OK(_driver->update(StringData(), _doc.get(), NULL));

        ASSERT_TRUE(_driver->modsAffectShardKeys());

        // Should be rejected, we are changing the value of a shard key.
        ASSERT_NOT_OK(_driver->checkShardKeysUnaltered(*_obj, *_doc));
    }

    TEST_F(ShardKeyTest, MutatingShardKeyFieldRejectedObjectReplace) {
        BSONObj mod(fromjson("{ x : [1], s : { a : [2], b : [2], c : [ 3, 3, 3 ] } }"));
        ASSERT_OK(_driver->parse(mod));
        _driver->refreshShardKeyPattern(_shardKeyPattern);
        ASSERT_OK(_driver->update(StringData(), _doc.get(), NULL));

        ASSERT_TRUE(_driver->modsAffectShardKeys());

        // Should be rejected, we are changing the value of a shard key.
        ASSERT_NOT_OK(_driver->checkShardKeysUnaltered(*_obj, *_doc));
    }

    TEST_F(ShardKeyTest, SettingShardKeyFieldToSameValueIsNotRejected) {
        BSONObj mod(fromjson("{ $push : { 's.a' : { $each : [1], $slice : -1 } } }"));
        ASSERT_OK(_driver->parse(mod));
        _driver->refreshShardKeyPattern(_shardKeyPattern);
        ASSERT_OK(_driver->update(StringData(), _doc.get(), NULL));

        // It is a no-op, so we don't see it as affecting.
        ASSERT_TRUE(_driver->modsAffectShardKeys());

        // Should not be rejected: 's.a' has the same value as it did originally.
        ASSERT_OK(_driver->checkShardKeysUnaltered(*_obj, *_doc));
    }

    TEST_F(ShardKeyTest, UnsettingShardKeyFieldRejected) {
        BSONObj mod(fromjson("{ $unset : { 's.a' : 1 } }"));
        ASSERT_OK(_driver->parse(mod));
        _driver->refreshShardKeyPattern(_shardKeyPattern);
        ASSERT_OK(_driver->update(StringData(), _doc.get(), NULL));

        ASSERT_TRUE(_driver->modsAffectShardKeys());

        // Should be rejected, we are removing one of the shard key fields
        ASSERT_NOT_OK(_driver->checkShardKeysUnaltered(*_obj, *_doc));
    }

    TEST_F(ShardKeyTest, SettingShardKeyChildrenRejected) {
        BSONObj mod(fromjson("{ $set : { 's.c.0' : 0 } }"));
        ASSERT_OK(_driver->parse(mod));
        _driver->refreshShardKeyPattern(_shardKeyPattern);
        ASSERT_OK(_driver->update(StringData(), _doc.get(), NULL));

        ASSERT_TRUE(_driver->modsAffectShardKeys());

        // Should be rejected, we are setting a value subsumed under one of the shard keys.
        ASSERT_NOT_OK(_driver->checkShardKeysUnaltered(*_obj, *_doc));
    }

    TEST_F(ShardKeyTest, UnsettingShardKeyChildrenRejected) {
        BSONObj mod(fromjson("{ $unset : { 's.c.0' : 1 } }"));
        ASSERT_OK(_driver->parse(mod));
        _driver->refreshShardKeyPattern(_shardKeyPattern);
        ASSERT_OK(_driver->update(StringData(), _doc.get(), NULL));

        ASSERT_TRUE(_driver->modsAffectShardKeys());

        // Should be rejected, we are removing one of the shard key fields
        ASSERT_NOT_OK(_driver->checkShardKeysUnaltered(*_obj, *_doc));
    }

    TEST_F(ShardKeyTest, SettingShardKeyChildrenToSameValueIsNotRejected) {
        BSONObj mod(fromjson("{ $push : { 's.c' : { $each : [3], $slice : -3 } } }"));
        ASSERT_OK(_driver->parse(mod));
        _driver->refreshShardKeyPattern(_shardKeyPattern);
        ASSERT_OK(_driver->update(StringData(), _doc.get(), NULL));

        ASSERT_TRUE(_driver->modsAffectShardKeys());

        // Should not be rejected, we are setting a value subsumed under one of the shard keys,
        // but the set is a logical no-op.
        ASSERT_OK(_driver->checkShardKeysUnaltered(*_obj, *_doc));
    }

    TEST_F(ShardKeyTest, AppendingToShardKeyChildrenRejected) {
        BSONObj mod(fromjson("{ $push : { 's.c' : 4 } }"));
        ASSERT_OK(_driver->parse(mod));
        _driver->refreshShardKeyPattern(_shardKeyPattern);
        ASSERT_OK(_driver->update(StringData(), _doc.get(), NULL));

        ASSERT_TRUE(_driver->modsAffectShardKeys());

        // Should be rejected, we are adding a new child under one of the shard keys.
        ASSERT_NOT_OK(_driver->checkShardKeysUnaltered(*_obj, *_doc));
    }

    TEST_F(ShardKeyTest, ModificationsToUnrelatedFieldsAreOK) {
        BSONObj mod(fromjson("{ $set : { x : 2, 's.b' : 'x' } }"));
        ASSERT_OK(_driver->parse(mod));
        _driver->refreshShardKeyPattern(_shardKeyPattern);
        ASSERT_OK(_driver->update(StringData(), _doc.get(), NULL));

        // Should not claim to have affected shard keys
        ASSERT_FALSE(_driver->modsAffectShardKeys());
    }

    TEST_F(ShardKeyTest, RemovingUnrelatedFieldsIsOK) {
        BSONObj mod(fromjson("{ $unset : { x : 1, 's.b' : 1 } }"));
        ASSERT_OK(_driver->parse(mod));
        _driver->refreshShardKeyPattern(_shardKeyPattern);
        ASSERT_OK(_driver->update(StringData(), _doc.get(), NULL));

        // Should not claim to have affected shard keys
        ASSERT_FALSE(_driver->modsAffectShardKeys());
    }

    TEST_F(ShardKeyTest, AddingUnrelatedFieldsIsOK) {
        BSONObj mod(fromjson("{ $set : { z : 1 } }"));
        ASSERT_OK(_driver->parse(mod));
        _driver->refreshShardKeyPattern(_shardKeyPattern);
        ASSERT_OK(_driver->update(StringData(), _doc.get(), NULL));

        // Should not claim to have affected shard keys
        ASSERT_FALSE(_driver->modsAffectShardKeys());
    }

    TEST_F(ShardKeyTest, OverwriteShardKeyFieldWithSameValueIsNotAnErrorObjectReplace) {
        BSONObj mod(fromjson("{ x : [1], s : { a : [1], b : [2], c : [ 3, 3, 3 ] } }"));
        ASSERT_OK(_driver->parse(mod));
        _driver->refreshShardKeyPattern(_shardKeyPattern);
        ASSERT_OK(_driver->update(StringData(), _doc.get(), NULL));

        ASSERT_TRUE(_driver->modsAffectShardKeys());

        // Applying the above mod should be OK, since we didn't actually change any of the
        // shard key values.
        ASSERT_OK(_driver->checkShardKeysUnaltered(*_obj, *_doc));
    }

} // unnamed namespace
