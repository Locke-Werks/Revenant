// 47 CFR 18.301 Table 1: ISM centre plus or minus tolerance
Band{"ISM and unlicensed", "ISM 6.78", 6'765'000, 6'795'000, 6'780'000, "", false, kRegionUS, BandKind::kAllocation, BandColour::kIsm, "47 CFR 18.301", ""},
Band{"ISM and unlicensed", "ISM 13.56", 13'553'000, 13'567'000, 13'560'000, "", false, kRegionUS, BandKind::kAllocation, BandColour::kIsm, "47 CFR 18.301", ""},
Band{"ISM and unlicensed", "ISM 27.12", 26'957'000, 27'283'000, 27'120'000, "", false, kRegionUS, BandKind::kAllocation, BandColour::kIsm, "47 CFR 18.301", ""},
Band{"ISM and unlicensed", "ISM 40.68", 40'660'000, 40'700'000, 40'680'000, "", false, kRegionUS, BandKind::kAllocation, BandColour::kIsm, "47 CFR 18.301", ""},
Band{"ISM and unlicensed", "ISM 915", 902'000'000, 928'000'000, 915'000'000, "am", false, kRegionUSCA, BandKind::kAllocation, BandColour::kIsm, "47 CFR 18.301, 15.247; ISED RSS-247", ""},
// 18.301 gives 2400-2500 MHz for ISM; Part 15 spread spectrum stops at 2483.5
Band{"ISM and unlicensed", "ISM 2.4 GHz", 2'400'000'000, 2'500'000'000, 2'450'000'000, "", false, kRegionUS, BandKind::kAllocation, BandColour::kIsm, "47 CFR 18.301", ""},
Band{"ISM and unlicensed", "2.4 GHz Part 15", 2'400'000'000, 2'483'500'000, 2'441'750'000, "", false, kRegionUSCA, BandKind::kSubBand, BandColour::kIsm, "47 CFR 15.247; ISED RSS-247", ""},
Band{"ISM and unlicensed", "ISM 5.8 GHz", 5'725'000'000, 5'875'000'000, 5'800'000'000, "", false, kRegionUS, BandKind::kAllocation, BandColour::kIsm, "47 CFR 18.301", ""},
Band{"ISM and unlicensed", "ISM 24 GHz", 24'000'000'000, 24'250'000'000, 24'125'000'000, "", false, kRegionUS, BandKind::kAllocation, BandColour::kIsm, "47 CFR 18.301", ""},
Band{"ISM and unlicensed", "ISM 61 GHz", 61'000'000'000, 61'500'000'000, 61'250'000'000, "", false, kRegionUS, BandKind::kAllocation, BandColour::kIsm, "47 CFR 18.301", ""},

// 47 CFR 15.231 and ISED RSS-210 Annex A: periodic transmitters; 433.05-434.79 MHz is ISM in ITU Region 1 only (RR 5.138), not in Region 2
Band{"ISM and unlicensed", "ISM 433", 433'050'000, 434'790'000, 433'920'000, "am", false, kRegionUSCA, BandKind::kSubBand, BandColour::kIsm, "47 CFR 15.231; ISED RSS-210 Annex A", ""},
// 47 CFR 15.231(c), RSS-210 Annex A: nominal width, emission bandwidth at most 0.25% of the centre frequency
Band{"ISM and unlicensed", "Keyfob 315", 314'606'250, 315'393'750, 315'000'000, "am", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "47 CFR 15.231; ISED RSS-210 Annex A", "Part 15 devices"}, // nominal width
Band{"ISM and unlicensed", "TPMS 315", 314'606'250, 315'393'750, 315'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "47 CFR 15.231; ISED RSS-210 Annex A", "Part 15 devices"}, // nominal width
Band{"ISM and unlicensed", "Alarm 345", 344'568'750, 345'431'250, 345'000'000, "am", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "47 CFR 15.231; ISED RSS-210 Annex A", "Part 15 devices"}, // nominal width
// 47 CFR 15.247: LoRa US915 inside 902-928
Band{"ISM and unlicensed", "LoRa US915", 902'000'000, 928'000'000, 915'000'000, "", false, kRegionUSCA, BandKind::kSubBand, BandColour::kIsm, "47 CFR 15.247; ISED RSS-247", ""},

// 47 CFR 15.407: U-NII sub-bands; 6425-6525 and 6875-7125 MHz have no separate edges in the rule text
Band{"ISM and unlicensed", "U-NII-1", 5'150'000'000, 5'250'000'000, 5'200'000'000, "", false, kRegionUSCA, BandKind::kSubBand, BandColour::kIsm, "47 CFR 15.407; ISED RSS-247", ""},
Band{"ISM and unlicensed", "U-NII-2A", 5'250'000'000, 5'350'000'000, 5'300'000'000, "", false, kRegionUSCA, BandKind::kSubBand, BandColour::kIsm, "47 CFR 15.407; ISED RSS-247", ""},
Band{"ISM and unlicensed", "U-NII-2C", 5'470'000'000, 5'725'000'000, 5'597'500'000, "", false, kRegionUSCA, BandKind::kSubBand, BandColour::kIsm, "47 CFR 15.407; ISED RSS-247", ""},
Band{"ISM and unlicensed", "U-NII-3", 5'725'000'000, 5'850'000'000, 5'787'500'000, "", false, kRegionUSCA, BandKind::kSubBand, BandColour::kIsm, "47 CFR 15.407; ISED RSS-247", ""},
Band{"ISM and unlicensed", "U-NII-4", 5'850'000'000, 5'895'000'000, 5'872'500'000, "", false, kRegionUSCA, BandKind::kSubBand, BandColour::kIsm, "47 CFR 15.407; ISED RSS-247", ""},
Band{"ISM and unlicensed", "6 GHz", 5'925'000'000, 7'125'000'000, 6'525'000'000, "", false, kRegionUSCA, BandKind::kAllocation, BandColour::kIsm, "47 CFR 15.407; ISED RSS-248", ""},
Band{"ISM and unlicensed", "U-NII-5", 5'925'000'000, 6'425'000'000, 6'175'000'000, "", false, kRegionUS, BandKind::kSubBand, BandColour::kIsm, "47 CFR 15.407", ""},
Band{"ISM and unlicensed", "U-NII-7", 6'525'000'000, 6'875'000'000, 6'700'000'000, "", false, kRegionUS, BandKind::kSubBand, BandColour::kIsm, "47 CFR 15.407", ""},

// 47 CFR 15.255 and ISED RSS-210 Annex J: 57-71 GHz
Band{"ISM and unlicensed", "57-71 GHz", 57'000'000'000, 71'000'000'000, 64'000'000'000, "", false, kRegionUSCA, BandKind::kAllocation, BandColour::kIsm, "47 CFR 15.255; ISED RSS-210 Annex J", ""},


// IEEE 802.11: 2.4 GHz channel centres 2407 + 5n MHz, 20 MHz wide, inside 47 CFR 15.247 2400-2483.5 MHz
Band{"ISM and unlicensed", "Wi-Fi 1", 2'402'000'000, 2'422'000'000, 2'412'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 2.4 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 2", 2'407'000'000, 2'427'000'000, 2'417'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 2.4 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 3", 2'412'000'000, 2'432'000'000, 2'422'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 2.4 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 4", 2'417'000'000, 2'437'000'000, 2'427'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 2.4 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 5", 2'422'000'000, 2'442'000'000, 2'432'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 2.4 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 6", 2'427'000'000, 2'447'000'000, 2'437'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 2.4 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 7", 2'432'000'000, 2'452'000'000, 2'442'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 2.4 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 8", 2'437'000'000, 2'457'000'000, 2'447'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 2.4 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 9", 2'442'000'000, 2'462'000'000, 2'452'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 2.4 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 10", 2'447'000'000, 2'467'000'000, 2'457'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 2.4 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 11", 2'452'000'000, 2'472'000'000, 2'462'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 2.4 GHz"},

// IEEE 802.11: 5 GHz channel centres 5000 + 5n MHz, 20 MHz wide, only those inside U-NII-1, -2A, -2C and -3
Band{"ISM and unlicensed", "Wi-Fi 36", 5'170'000'000, 5'190'000'000, 5'180'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 40", 5'190'000'000, 5'210'000'000, 5'200'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 44", 5'210'000'000, 5'230'000'000, 5'220'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 48", 5'230'000'000, 5'250'000'000, 5'240'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 52", 5'250'000'000, 5'270'000'000, 5'260'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 56", 5'270'000'000, 5'290'000'000, 5'280'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 60", 5'290'000'000, 5'310'000'000, 5'300'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 64", 5'310'000'000, 5'330'000'000, 5'320'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 100", 5'490'000'000, 5'510'000'000, 5'500'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 104", 5'510'000'000, 5'530'000'000, 5'520'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 108", 5'530'000'000, 5'550'000'000, 5'540'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 112", 5'550'000'000, 5'570'000'000, 5'560'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 116", 5'570'000'000, 5'590'000'000, 5'580'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 120", 5'590'000'000, 5'610'000'000, 5'600'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 124", 5'610'000'000, 5'630'000'000, 5'620'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 128", 5'630'000'000, 5'650'000'000, 5'640'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 132", 5'650'000'000, 5'670'000'000, 5'660'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 136", 5'670'000'000, 5'690'000'000, 5'680'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 140", 5'690'000'000, 5'710'000'000, 5'700'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 144", 5'710'000'000, 5'730'000'000, 5'720'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 149", 5'735'000'000, 5'755'000'000, 5'745'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 153", 5'755'000'000, 5'775'000'000, 5'765'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 157", 5'775'000'000, 5'795'000'000, 5'785'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 161", 5'795'000'000, 5'815'000'000, 5'805'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
Band{"ISM and unlicensed", "Wi-Fi 165", 5'815'000'000, 5'835'000'000, 5'825'000'000, "", false, kRegionUSCA, BandKind::kChannel, BandColour::kIsm, "IEEE 802.11", "Wi-Fi 5 GHz"},
