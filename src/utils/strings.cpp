/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "strings.h"

std::string utils::trim(std::string input)
{
	input.resize(input.find_last_not_of("\n ") + 1);

	return input;
}

std::vector<std::string> utils::split(const std::string & s, const std::string & sep)
{
	std::string::size_type i = 0;
	std::vector<std::string> v;

	while (true)
	{
		std::string::size_type j = s.find_first_not_of(sep, i);
		if (j == std::string::npos)
		{
			v.push_back(s.substr(i + 1));
			return v;
		}

		v.push_back(s.substr(i, j - i));
		i = j + 1;
	}
}
