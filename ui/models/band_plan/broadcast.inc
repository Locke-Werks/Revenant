// LW broadcasting (148.5-283.5 kHz) is a Region 1 allocation only; Region 2 has none, so it is left out.

// 47 CFR 73.14: AM broadcast band 535-1705 kHz, carriers 540-1700 kHz; expanded band 1605-1705 kHz
Band{"Broadcast", "MW AM", 535'000, 1'705'000, 1'000'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 73.14", ""},
Band{"Broadcast", "AM expanded band", 1'605'000, 1'705'000, 1'650'000, "am", false, kRegionUSCA, BandKind::kSubBand, BandColour::kBroadcast, "47 CFR 73.14", ""},

// 47 CFR 73.201: FM 88-108 MHz, channels 201-300. Channels 201-220 (88.1-91.9 MHz) are the
// noncommercial educational channels per 73.501. FM translators share the band (Part 74 subpart L).
Band{"Broadcast", "FM", 88'000'000, 108'000'000, 98'100'000, "wfm", true, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 73.201", ""},
Band{"Broadcast", "FM noncommercial", 88'000'000, 92'000'000, 90'100'000, "wfm", false, kRegionUSCA, BandKind::kSubBand, BandColour::kBroadcast, "47 CFR 73.201", ""},

// 47 CFR 73.603(a): TV channels 2-36. Channel 37 (608-614 MHz) is radio astronomy, 73.603(c).
Band{"Broadcast", "TV VHF low", 54'000'000, 88'000'000, 57'000'000, "", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 73.603", ""},
Band{"Broadcast", "TV 2", 54'000'000, 60'000'000, 57'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV VHF"},
Band{"Broadcast", "TV 3", 60'000'000, 66'000'000, 63'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV VHF"},
Band{"Broadcast", "TV 4", 66'000'000, 72'000'000, 69'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV VHF"},
Band{"Broadcast", "TV 5", 76'000'000, 82'000'000, 79'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV VHF"},
Band{"Broadcast", "TV 6", 82'000'000, 88'000'000, 85'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV VHF"},
Band{"Broadcast", "TV VHF high", 174'000'000, 216'000'000, 195'000'000, "", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 73.603", ""},
Band{"Broadcast", "TV 7", 174'000'000, 180'000'000, 177'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV VHF"},
Band{"Broadcast", "TV 8", 180'000'000, 186'000'000, 183'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV VHF"},
Band{"Broadcast", "TV 9", 186'000'000, 192'000'000, 189'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV VHF"},
Band{"Broadcast", "TV 10", 192'000'000, 198'000'000, 195'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV VHF"},
Band{"Broadcast", "TV 11", 198'000'000, 204'000'000, 201'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV VHF"},
Band{"Broadcast", "TV 12", 204'000'000, 210'000'000, 207'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV VHF"},
Band{"Broadcast", "TV 13", 210'000'000, 216'000'000, 213'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV VHF"},
Band{"Broadcast", "TV UHF", 470'000'000, 608'000'000, 539'000'000, "", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 73.603", ""},
Band{"Broadcast", "TV 14", 470'000'000, 476'000'000, 473'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 15", 476'000'000, 482'000'000, 479'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 16", 482'000'000, 488'000'000, 485'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 17", 488'000'000, 494'000'000, 491'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 18", 494'000'000, 500'000'000, 497'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 19", 500'000'000, 506'000'000, 503'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 20", 506'000'000, 512'000'000, 509'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 21", 512'000'000, 518'000'000, 515'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 22", 518'000'000, 524'000'000, 521'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 23", 524'000'000, 530'000'000, 527'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 24", 530'000'000, 536'000'000, 533'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 25", 536'000'000, 542'000'000, 539'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 26", 542'000'000, 548'000'000, 545'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 27", 548'000'000, 554'000'000, 551'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 28", 554'000'000, 560'000'000, 557'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 29", 560'000'000, 566'000'000, 563'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 30", 566'000'000, 572'000'000, 569'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 31", 572'000'000, 578'000'000, 575'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 32", 578'000'000, 584'000'000, 581'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 33", 584'000'000, 590'000'000, 587'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 34", 590'000'000, 596'000'000, 593'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 35", 596'000'000, 602'000'000, 599'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},
Band{"Broadcast", "TV 36", 602'000'000, 608'000'000, 605'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kBroadcast, "47 CFR 73.603", "TV UHF"},

// FCC broadcast incentive auction (Auction 1002): 614-698 MHz left TV for the 600 MHz band.
// 47 CFR 27.5(l): downlink 617-652 MHz, uplink 663-698 MHz; NG33 in 47 CFR 2.106. Canada is in canada_only.inc.
Band{"Broadcast", "600 MHz mobile", 614'000'000, 698'000'000, 634'500'000, "", false, kRegionUS, BandKind::kAllocation, BandColour::kLandMobile, "47 CFR 27.5(l)", ""},
Band{"Broadcast", "600 MHz downlink", 617'000'000, 652'000'000, 634'500'000, "", false, kRegionUS, BandKind::kSubBand, BandColour::kLandMobile, "47 CFR 27.5(l)", ""},
Band{"Broadcast", "600 MHz uplink", 663'000'000, 698'000'000, 680'500'000, "", false, kRegionUS, BandKind::kSubBand, BandColour::kLandMobile, "47 CFR 27.5(l)", ""},
Band{"Broadcast", "600 MHz duplex gap mics", 653'000'000, 657'000'000, 655'000'000, "", false, kRegionUS, BandKind::kSubBand, BandColour::kBroadcast, "47 CFR 74.802", ""},

// Broadcast auxiliary. 74.402 lists RPU channel centres; the HF row spans the first and last centre.
Band{"Broadcast", "RPU 26 MHz", 25'870'000, 26'470'000, 26'170'000, "am", false, kRegionUS, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 74.402(a)(2)", ""},
Band{"Broadcast", "Wireless mics 26 MHz", 26'100'000, 26'480'000, 26'290'000, "nfm", false, kRegionUS, BandKind::kSubBand, BandColour::kBroadcast, "47 CFR 74.802", ""},
Band{"Broadcast", "RPU 161 MHz", 161'625'000, 161'775'000, 161'700'000, "nfm", false, kRegionUS, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 74.402(b)(3)", ""},
Band{"Broadcast", "RPU 450 MHz", 450'000'000, 451'000'000, 450'500'000, "nfm", false, kRegionUS, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 74.802", ""},
Band{"Broadcast", "RPU 455 MHz", 455'000'000, 456'000'000, 455'500'000, "nfm", false, kRegionUS, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 74.802", ""},

// Tropical bands, Region 2: 47 CFR 2.106 footnote 5.113
Band{"Shortwave broadcast", "SW 120 m", 2'300'000, 2'495'000, 2'400'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 2.106 fn 5.113", ""},
Band{"Shortwave broadcast", "SW 90 m", 3'200'000, 3'400'000, 3'300'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 2.106 fn 5.113", ""},
Band{"Shortwave broadcast", "SW 60 m", 4'750'000, 4'995'000, 4'900'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 2.106 fn 5.113", ""},
Band{"Shortwave broadcast", "SW 60 m upper", 5'005'000, 5'060'000, 5'030'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 2.106 fn 5.113", ""},
// 47 CFR 73.702: Region 2 HF broadcast bands. 41 m is 7300-7350 exclusive plus 7350-7400 co-primary;
// 7200-7300 and 7400-7450 are not broadcast in Region 2. 75 m is not a Region 2 broadcast band.
Band{"Shortwave broadcast", "SW 49 m", 5'900'000, 6'200'000, 6'050'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 73.702", ""},
Band{"Shortwave broadcast", "SW 41 m", 7'300'000, 7'400'000, 7'350'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 73.702", ""},
Band{"Shortwave broadcast", "SW 31 m", 9'400'000, 9'900'000, 9'650'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 73.702", ""},
Band{"Shortwave broadcast", "SW 25 m", 11'600'000, 12'100'000, 11'850'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 73.702", ""},
Band{"Shortwave broadcast", "SW 22 m", 13'570'000, 13'870'000, 13'720'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 73.702", ""},
Band{"Shortwave broadcast", "SW 19 m", 15'100'000, 15'800'000, 15'450'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 73.702", ""},
Band{"Shortwave broadcast", "SW 16 m", 17'480'000, 17'900'000, 17'690'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 73.702", ""},
Band{"Shortwave broadcast", "SW 15 m", 18'900'000, 19'020'000, 18'960'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 73.702", ""},
Band{"Shortwave broadcast", "SW 13 m", 21'450'000, 21'850'000, 21'650'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 73.702", ""},
Band{"Shortwave broadcast", "SW 11 m", 25'670'000, 26'100'000, 25'885'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kBroadcast, "47 CFR 73.702", ""},
