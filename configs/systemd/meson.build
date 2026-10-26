# Copyright (C) 2019 by Sukchan Lee <acetcom@gmail.com>

# This file is part of xcn.

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

systemd_conf_in = '''
    xcn-mmed.service
    xcn-sgwcd.service
    xcn-smfd.service
    xcn-amfd.service
    xcn-sgwud.service
    xcn-upfd.service
    xcn-hssd.service
    xcn-pcrfd.service
    xcn-nrfd.service
    xcn-scpd.service
    xcn-seppd.service
    xcn-ausfd.service
    xcn-udmd.service
    xcn-pcfd.service
    xcn-nssfd.service
    xcn-bsfd.service
    xcn-udrd.service
'''.split()

foreach file : systemd_conf_in
    configure_file(
            input : file + '.in',
            output : file,
            configuration : conf_data)
endforeach

systemd_conf = '''
    99-xcn.netdev
    99-xcn.network
'''.split()

foreach file : systemd_conf
    configure_file(
            input : file,
            output : file,
            configuration : conf_data)
endforeach
