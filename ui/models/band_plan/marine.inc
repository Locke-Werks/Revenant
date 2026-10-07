// 510-525 kHz maritime mobile: 47 CFR 2.106 (ships only, RR 5.79A 5.84) and ISED RBR-2 Schedule II
Band{"Marine", "NAVTEX band", 510'000, 525'000, 518'000, "", false, kRegionUSCA, BandKind::kSubBand, BandColour::kMarine, "47 CFR 2.106; ISED RBR-2", ""},
// NAVTEX 490, 518 and 4209.5 kHz: RR 5.79A in 47 CFR 2.106 and the Canadian Table
Band{"Marine", "NAVTEX 490", 489'750, 490'250, 490'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 2.106; ISED Canadian Table", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NAVTEX 518", 517'750, 518'250, 518'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 2.106; ISED Canadian Table", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NAVTEX 4209.5", 4'209'250, 4'209'750, 4'209'500, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 2.106; ISED Canadian Table", "Marine HF broadcasts"}, // nominal width
// 2173.5-2190.5 kHz mobile (distress and calling): 47 CFR 2.106 and ISED RBR-2 Schedule II
Band{"Marine", "2 MHz distress", 2'173'500, 2'190'500, 2'182'000, "usb", false, kRegionUSCA, BandKind::kSubBand, BandColour::kMarine, "47 CFR 2.106; ISED RBR-2", ""},
// 2182 kHz: 47 CFR 87.173 and RBR-2 Sched. III; the assigned frequency is carrier + 1.4 kHz (RBR-2 Sched. III note 3), so the J3E channel is carrier to carrier + 2.8 kHz
Band{"Marine", "2182 distress", 2'182'000, 2'184'800, 2'182'000, "usb", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine distress"},
// HF maritime mobile bands: 47 CFR 2.106 and ISED RBR-2 Schedule II
Band{"Marine", "Marine HF 4.063 MHz", 4'063'000, 4'438'000, 4'250'500, "usb", false, kRegionUSCA, BandKind::kAllocation, BandColour::kMarine, "47 CFR 2.106; ISED RBR-2", ""},
Band{"Marine", "Marine HF 6.2 MHz", 6'200'000, 6'525'000, 6'362'500, "usb", false, kRegionUSCA, BandKind::kAllocation, BandColour::kMarine, "47 CFR 2.106; ISED RBR-2", ""},
Band{"Marine", "Marine HF 8.195 MHz", 8'195'000, 8'815'000, 8'505'000, "usb", false, kRegionUSCA, BandKind::kAllocation, BandColour::kMarine, "47 CFR 2.106; ISED RBR-2", ""},
Band{"Marine", "Marine HF 12.23 MHz", 12'230'000, 13'200'000, 12'715'000, "usb", false, kRegionUSCA, BandKind::kAllocation, BandColour::kMarine, "47 CFR 2.106; ISED RBR-2", ""},
Band{"Marine", "Marine HF 16.36 MHz", 16'360'000, 17'410'000, 16'885'000, "usb", false, kRegionUSCA, BandKind::kAllocation, BandColour::kMarine, "47 CFR 2.106; ISED RBR-2", ""},
Band{"Marine", "Marine HF 18.78 MHz", 18'780'000, 18'900'000, 18'840'000, "usb", false, kRegionUSCA, BandKind::kAllocation, BandColour::kMarine, "47 CFR 2.106; ISED RBR-2", ""},
Band{"Marine", "Marine HF 19.68 MHz", 19'680'000, 19'800'000, 19'740'000, "usb", false, kRegionUSCA, BandKind::kAllocation, BandColour::kMarine, "47 CFR 2.106; ISED RBR-2", ""},
Band{"Marine", "Marine HF 22 MHz", 22'000'000, 22'855'000, 22'427'500, "usb", false, kRegionUSCA, BandKind::kAllocation, BandColour::kMarine, "47 CFR 2.106; ISED RBR-2", ""},
Band{"Marine", "Marine HF 25.07 MHz", 25'070'000, 25'210'000, 25'140'000, "usb", false, kRegionUSCA, BandKind::kAllocation, BandColour::kMarine, "47 CFR 2.106; ISED RBR-2", ""},
Band{"Marine", "Marine HF 26.1 MHz", 26'100'000, 26'175'000, 26'137'500, "usb", false, kRegionUSCA, BandKind::kAllocation, BandColour::kMarine, "47 CFR 2.106; ISED RBR-2", ""},
// DSC distress and safety: 47 CFR 80.359(b); RBR-2 Schedule III lists all but 16804.5
Band{"Marine", "DSC 2187.5", 2'187'250, 2'187'750, 2'187'500, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.359; ISED RBR-2", "DSC"}, // nominal width
Band{"Marine", "DSC 4207.5", 4'207'250, 4'207'750, 4'207'500, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.359; ISED RBR-2", "DSC"}, // nominal width
Band{"Marine", "DSC 6312", 6'311'750, 6'312'250, 6'312'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.359; ISED RBR-2", "DSC"}, // nominal width
Band{"Marine", "DSC 8414.5", 8'414'250, 8'414'750, 8'414'500, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.359; ISED RBR-2", "DSC"}, // nominal width
Band{"Marine", "DSC 12577", 12'576'750, 12'577'250, 12'577'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.359; ISED RBR-2", "DSC"}, // nominal width
Band{"Marine", "DSC 16804.5", 16'804'250, 16'804'750, 16'804'500, "", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.359", "DSC"}, // nominal width
// USCG radiofax, NWS listing of USCG broadcasts. Listed frequencies are assigned; the USB carrier is 1.9 kHz lower,
// so each row runs from the carrier up 3 kHz with the carrier as the tune target
Band{"Marine", "NMF fax 4235", 4'233'100, 4'236'100, 4'233'100, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NMF fax 6340.5", 6'338'600, 6'341'600, 6'338'600, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NMF fax 9110", 9'108'100, 9'111'100, 9'108'100, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NMF fax 12750", 12'748'100, 12'751'100, 12'748'100, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NMG fax 4317.9", 4'316'000, 4'319'000, 4'316'000, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NMG fax 8503.9", 8'502'000, 8'505'000, 8'502'000, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NMG fax 12789.9", 12'788'000, 12'791'000, 12'788'000, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NMG fax 17146.4", 17'144'500, 17'147'500, 17'144'500, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NOJ fax 2054", 2'052'100, 2'055'100, 2'052'100, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NOJ fax 4298", 4'296'100, 4'299'100, 4'296'100, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NOJ fax 8459", 8'457'100, 8'460'100, 8'457'100, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NOJ fax 12410.6", 12'408'700, 12'411'700, 12'408'700, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NMC fax 4346", 4'344'100, 4'347'100, 4'344'100, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NMC fax 8682", 8'680'100, 8'683'100, 8'680'100, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NMC fax 12786", 12'784'100, 12'787'100, 12'784'100, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NMC fax 17151.2", 17'149'300, 17'152'300, 17'149'300, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "NMC fax 22527", 22'525'100, 22'528'100, 22'525'100, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
// USCG HF voice weather, USB carrier frequencies (NMN, NMC, NOJ and others share 4426 to 17314; NMG 4316, 8502, 12788)
Band{"Marine", "USCG voice 4316", 4'316'000, 4'319'000, 4'316'000, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "USCG voice 4426", 4'426'000, 4'429'000, 4'426'000, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "USCG voice 6501", 6'501'000, 6'504'000, 6'501'000, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "USCG voice 8502", 8'502'000, 8'505'000, 8'502'000, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "USCG voice 8764", 8'764'000, 8'767'000, 8'764'000, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "USCG voice 12788", 12'788'000, 12'791'000, 12'788'000, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "USCG voice 13089", 13'089'000, 13'092'000, 13'089'000, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "USCG voice 17314", 17'314'000, 17'317'000, 17'314'000, "usb", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
// USCG HF SITOR (NBDP) broadcasts
Band{"Marine", "SITOR 6314", 6'313'750, 6'314'250, 6'314'000, "", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "SITOR 8416.5", 8'416'250, 8'416'750, 8'416'500, "", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "SITOR 12579", 12'578'750, 12'579'250, 12'579'000, "", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "SITOR 16806.5", 16'806'250, 16'806'750, 16'806'500, "", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
Band{"Marine", "SITOR 22376", 22'375'750, 22'376'250, 22'376'000, "", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "weather.gov/marine/uscg_broadcasts", "Marine HF broadcasts"}, // nominal width
// Name and edges kept from the original plan
Band{"Marine", "Marine VHF", 156'000'000, 162'025'000, 156'800'000, "nfm", false, kRegionUSCA, BandKind::kAllocation, BandColour::kMarine, "47 CFR 80.373", ""},
// VHF channels, 25 kHz channels (47 CFR 80.371(c)(1)(iii)). Centre is the frequency a ship receives:
// the simplex frequency, or the coast transmit frequency of a duplex pair. US: 47 CFR 80.371(c), 80.373(f).
// Canada: ISED RBR-2 Schedule I. A channel identical in both is one row.
Band{"Marine", "Ch 01", 160'637'500, 160'662'500, 160'650'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 01A", 156'037'500, 156'062'500, 156'050'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373", "Marine VHF channels"},
Band{"Marine", "Ch 02", 160'687'500, 160'712'500, 160'700'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 03", 160'737'500, 160'762'500, 160'750'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 04A", 156'187'500, 156'212'500, 156'200'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 05A", 156'237'500, 156'262'500, 156'250'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 06", 156'287'500, 156'312'500, 156'300'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 07A", 156'337'500, 156'362'500, 156'350'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 08", 156'387'500, 156'412'500, 156'400'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 09", 156'437'500, 156'462'500, 156'450'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 10", 156'487'500, 156'512'500, 156'500'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 11", 156'537'500, 156'562'500, 156'550'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 12", 156'587'500, 156'612'500, 156'600'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 13", 156'637'500, 156'662'500, 156'650'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 14", 156'687'500, 156'712'500, 156'700'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 15", 156'737'500, 156'762'500, 156'750'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 16", 156'787'500, 156'812'500, 156'800'000, "nfm", true, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 17", 156'837'500, 156'862'500, 156'850'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 18A", 156'887'500, 156'912'500, 156'900'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 19A", 156'937'500, 156'962'500, 156'950'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 20", 161'587'500, 161'612'500, 161'600'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 20A", 156'987'500, 157'012'500, 157'000'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373", "Marine VHF channels"},
Band{"Marine", "Ch 21A", 157'037'500, 157'062'500, 157'050'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 21B", 161'637'500, 161'662'500, 161'650'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 22A", 157'087'500, 157'112'500, 157'100'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 23", 161'737'500, 161'762'500, 161'750'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 23B", 161'737'500, 161'762'500, 161'750'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 24", 161'787'500, 161'812'500, 161'800'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.371; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 25", 161'837'500, 161'862'500, 161'850'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.371; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 25B", 161'837'500, 161'862'500, 161'850'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 26", 161'887'500, 161'912'500, 161'900'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.371; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 27", 161'937'500, 161'962'500, 161'950'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.371; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 28", 161'987'500, 162'012'500, 162'000'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.371; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 28B", 161'987'500, 162'012'500, 162'000'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 60", 160'612'500, 160'637'500, 160'625'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 61A", 156'062'500, 156'087'500, 156'075'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 62A", 156'112'500, 156'137'500, 156'125'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 63A", 156'162'500, 156'187'500, 156'175'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 64", 160'812'500, 160'837'500, 160'825'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 64A", 156'212'500, 156'237'500, 156'225'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 65A", 156'262'500, 156'287'500, 156'275'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 66A", 156'312'500, 156'337'500, 156'325'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 67", 156'362'500, 156'387'500, 156'375'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 68", 156'412'500, 156'437'500, 156'425'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 69", 156'462'500, 156'487'500, 156'475'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 70", 156'512'500, 156'537'500, 156'525'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 71", 156'562'500, 156'587'500, 156'575'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 72", 156'612'500, 156'637'500, 156'625'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 73", 156'662'500, 156'687'500, 156'675'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 74", 156'712'500, 156'737'500, 156'725'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 75", 156'762'500, 156'787'500, 156'775'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 76", 156'812'500, 156'837'500, 156'825'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 77", 156'862'500, 156'887'500, 156'875'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 78A", 156'912'500, 156'937'500, 156'925'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 79A", 156'962'500, 156'987'500, 156'975'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 80A", 157'012'500, 157'037'500, 157'025'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 81A", 157'062'500, 157'087'500, 157'075'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 82A", 157'112'500, 157'137'500, 157'125'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 83A", 157'162'500, 157'187'500, 157'175'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 83B", 161'762'500, 161'787'500, 161'775'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 84", 161'812'500, 161'837'500, 161'825'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.371; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 85", 161'862'500, 161'887'500, 161'875'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.371; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 86", 161'912'500, 161'937'500, 161'925'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.371; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 87", 157'362'500, 157'387'500, 157'375'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "AIS 1 (87B)", 161'962'500, 161'987'500, 161'975'000, "raw", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.371; ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 88", 157'412'500, 157'437'500, 157'425'000, "nfm", false, kRegionCA, BandKind::kChannel, BandColour::kMarine, "ISED RBR-2", "Marine VHF channels"},
Band{"Marine", "Ch 88A", 157'412'500, 157'437'500, 157'425'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.373", "Marine VHF channels"},
Band{"Marine", "AIS 2 (88B)", 162'012'500, 162'037'500, 162'025'000, "raw", false, kRegionUSCA, BandKind::kChannel, BandColour::kMarine, "47 CFR 80.371; ISED RBR-2", "Marine VHF channels"},
