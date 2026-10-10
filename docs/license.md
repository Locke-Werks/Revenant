# License history

This was all rights reserved, held open on the reasoning that the choice
between permissive, copyleft, dual and source-available is made once and was
better made later. Getting a radio working settled it. librtlsdr is the only
practical path to an RTL2832U, it is GPL-2.0-or-later, and the clean-room
route was measured and found blocked: the datasheet specifies the transport
and contains no raw IQ mode at all, because that mode was discovered by
sniffing in 2012 and exists as one project's expression of it. The evidence
is in [docs/rtlsdr-provenance.md](rtlsdr-provenance.md).

The version is GPL-3.0 rather than 2.0 because librtlsdr is "or later", and
that matters more than it looks: GPL-2.0-only cannot be combined with
LGPL-3.0, which is Qt6, so a 2.0-only dependency would have killed the user
interface along with the licence question.

Relicensing is not publishing, and the two happened at different times here.
Copyleft obligations attach to distribution, so a repository whose binaries
stay on their author's machines owes nothing to anyone; the duty to offer
corresponding source begins when a binary is handed to someone else. Binaries
have been handed out since v0.1.0 on 2026-09-27, and every release page
carries the corresponding source beside the installer:
`Revenant-<version>-corresponding-source.zip` for Revenant, libusb and
librtlsdr, and the Qt and FFmpeg source archives the client is built from,
listed in `SOURCES-client.txt`. The release obligations are in
[docs/clean-room.md](clean-room.md).

Clean-room is still the default everywhere it is affordable, which is
everywhere a specification is published. The policy, the exceptions and the
reasoning are in [docs/clean-room.md](clean-room.md).
