#ifndef SCHEMA_CERT_HPP
#define SCHEMA_CERT_HPP
/*
 * schemaCert - construct a signed cert containing a schema
 *
 * Copyright (C) 2020 Pollere, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <https://www.gnu.org/licenses/>.
 *  You may contact Pollere, Inc at info@pollere.net.
 *
 *  The DCT proof-of-concept is not intended as production code.
 *  More information on DCT is available from info@pollere.net
 */

#include "dct/format.hpp"
#include "dct/schema/dct_cert.hpp"
#include "dct/sigmgrs/sigmgr_by_type.hpp"

static inline auto schemaCert(const bSchema& bs, const keyVal& pk, SigMgr& sm) {
    // schema must be signed the same way as its pubs
    auto valtype = bs.pubVal("#pubValidator").substr(1);

    // schema names are derived from the schema and must have the form:
    //   <pubPrefix>/"schema"/<pubName>/"KEY"/<kId>/"self"/<creationTime>
    // where <pubName> is the name of the first pub in the schema.
    auto certName = format("{}/schema/{}", bs.pubVal("#pubPrefix"), bs.pubName(0));
    return dctCert(certName, pk, sm);
}

#endif // SCHEMA_CERT_HPP