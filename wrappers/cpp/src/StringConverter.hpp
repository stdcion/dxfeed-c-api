/*
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Initial Developer of the Original Code is Devexperts LLC.
 * Portions created by the Initial Developer are Copyright (C) 2010
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 */

#pragma once

#include <locale>
#include <string>

#ifdef _MSC_FULL_VER
#	pragma warning( push )
#	pragma warning( disable : 4244 )
#endif

struct StringConverter {
	static std::string toString(const std::wstring &wstring) {
		return std::string(wstring.begin(), wstring.end());
	}

	static std::string toString(wchar_t wchar) {
		return toString(std::wstring(1, wchar));
	}

	template <typename InputIterator>
	static std::string toString(InputIterator first, InputIterator last) {
		return toString(std::wstring(first, last));
	}

	static std::wstring toWString(const std::string &string) {
		return std::wstring(string.begin(), string.end());
	}
};

#ifdef _MSC_FULL_VER
#	pragma warning( pop )
#endif
