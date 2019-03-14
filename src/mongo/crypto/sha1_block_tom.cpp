/*
 *    Copyright (C) 2014 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/crypto/sha1_block.h"

#include "mongo/config.h"
#include "mongo/util/assert_util.h"

#ifdef MONGO_CONFIG_SSL
#error This file should not be included if compiling with SSL support
#endif

#include "mongo/crypto/tom/tomcrypt.h"

namespace mongo {

/*
 * Computes a SHA-1 hash of 'input'.
 */
SHA1Block SHA1Block::computeHash(const uint8_t* input, size_t inputLen) {
    HashType output;

    hash_state hashState;
    fassert(40381,
            sha1_init(&hashState) == CRYPT_OK &&
                sha1_process(&hashState, input, inputLen) == CRYPT_OK &&
                sha1_done(&hashState, output.data()) == CRYPT_OK);
    return SHA1Block(output);
}

/*
 * Computes a HMAC SHA-1 keyed hash of 'input' using the key 'key'
 */
void SHA1Block::computeHmac(const uint8_t* key,
                            size_t keyLen,
                            const uint8_t* input,
                            size_t inputLen,
                            SHA1Block* const output) {
    invariant(key && input);

    static int hashId = -1;
    if (hashId == -1) {
        register_hash(&sha1_desc);
        hashId = find_hash("sha1");
    }

    unsigned long sha1HashLen = 20;
    fassert(40382,
            hmac_memory(hashId, key, keyLen, input, inputLen, output->_hash.data(), &sha1HashLen) ==
                CRYPT_OK);
}

}  // namespace mongo
