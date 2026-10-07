// NOAA Weather Radio; Environment Canada Weatheradio uses the same seven frequencies
Band{"Weather", "NOAA", 162'400'000, 162'550'000, 162'475'000, "nfm", true, kRegionUSCA, BandKind::kAllocation, BandColour::kWeather, "NOAA NWR; ECCC Weatheradio", ""},
Band{"Weather", "WX1", 162'537'500, 162'562'500, 162'550'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kWeather, "NOAA NWR; ECCC Weatheradio", "NOAA weather"}, // nominal width
Band{"Weather", "WX2", 162'387'500, 162'412'500, 162'400'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kWeather, "NOAA NWR; ECCC Weatheradio", "NOAA weather"}, // nominal width
Band{"Weather", "WX3", 162'462'500, 162'487'500, 162'475'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kWeather, "NOAA NWR; ECCC Weatheradio", "NOAA weather"}, // nominal width
Band{"Weather", "WX4", 162'412'500, 162'437'500, 162'425'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kWeather, "NOAA NWR; ECCC Weatheradio", "NOAA weather"}, // nominal width
Band{"Weather", "WX5", 162'437'500, 162'462'500, 162'450'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kWeather, "NOAA NWR; ECCC Weatheradio", "NOAA weather"}, // nominal width
Band{"Weather", "WX6", 162'487'500, 162'512'500, 162'500'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kWeather, "NOAA NWR; ECCC Weatheradio", "NOAA weather"}, // nominal width
Band{"Weather", "WX7", 162'512'500, 162'537'500, 162'525'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kWeather, "NOAA NWR; ECCC Weatheradio", "NOAA weather"}, // nominal width
// Radiosondes: NWS moved from 1675-1683 MHz to 400.15-406 MHz for GOES-R GRB
Band{"Weather", "Radiosonde 403", 400'150'000, 406'000'000, 403'000'000, "nfm", false, kRegionUSCA, BandKind::kAllocation, BandColour::kWeather, "47 CFR 2.106", ""},
Band{"Weather", "Radiosonde 1680", 1'675'000'000, 1'683'000'000, 1'680'000'000, "nfm", false, kRegionUS, BandKind::kAllocation, BandColour::kWeather, "NOAA NWS; 47 CFR 2.106", ""},
// NOAA POES APT, decommissioned 2025 (NOAA-19 13 Aug, NOAA-15 19 Aug), kept for recordings
// 137-138 MHz: meteorological-satellite and mobile-satellite (space-to-Earth), Orbcomm downlink per FCC DA 95-1309
Band{"Weather", "Weather sat 137", 137'000'000, 138'000'000, 137'500'000, "wfm", false, kRegionUSCA, BandKind::kAllocation, BandColour::kWeather, "47 CFR 2.106; NTIA", ""},
Band{"Satellite and space", "Orbcomm", 137'000'000, 138'000'000, 137'500'000, "raw", false, kRegionUSCA, BandKind::kAllocation, BandColour::kSatellite, "FCC DA 95-1309", ""},
Band{"Weather", "NOAA 15 APT", 137'603'000, 137'637'000, 137'620'000, "wfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kWeather, "NOAA OSPO", "Weather satellites"}, // nominal width
Band{"Weather", "NOAA 18 APT", 137'895'500, 137'929'500, 137'912'500, "wfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kWeather, "NOAA OSPO", "Weather satellites"}, // nominal width
Band{"Weather", "NOAA 19 APT", 137'083'000, 137'117'000, 137'100'000, "wfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kWeather, "NOAA OSPO", "Weather satellites"}, // nominal width
// WMO OSCAR: METEOR-M LRPT, 150 kHz bandwidth
Band{"Weather", "METEOR LRPT 137.1", 137'025'000, 137'175'000, 137'100'000, "raw", false, kRegionUSCA, BandKind::kChannel, BandColour::kWeather, "WMO OSCAR", "Weather satellites"},
Band{"Weather", "METEOR LRPT 137.9", 137'825'000, 137'975'000, 137'900'000, "raw", false, kRegionUSCA, BandKind::kChannel, BandColour::kWeather, "WMO OSCAR", "Weather satellites"},
Band{"Weather", "GOES HRIT", 1'693'500'000, 1'694'700'000, 1'694'100'000, "raw", false, kRegionUSCA, BandKind::kChannel, BandColour::kWeather, "NOAA GOES-R HRIT/EMWIN", "Weather satellites"}, // nominal width
Band{"Satellite and space", "Inmarsat L-band", 1'525'000'000, 1'559'000'000, 1'542'000'000, "raw", false, kRegionUSCA, BandKind::kAllocation, BandColour::kSatellite, "47 CFR 2.106", ""},
Band{"Satellite and space", "Iridium", 1'616'000'000, 1'626'500'000, 1'621'250'000, "raw", false, kRegionUSCA, BandKind::kAllocation, BandColour::kSatellite, "47 CFR 2.106", ""},
// GNSS signal centres; widths are the published signal bandwidths
Band{"Satellite and space", "GPS L1", 1'563'420'000, 1'587'420'000, 1'575'420'000, "raw", false, kRegionUSCA, BandKind::kChannel, BandColour::kSatellite, "IS-GPS-200", "GNSS"}, // nominal width
Band{"Satellite and space", "GPS L2", 1'215'600'000, 1'239'600'000, 1'227'600'000, "raw", false, kRegionUSCA, BandKind::kChannel, BandColour::kSatellite, "IS-GPS-200", "GNSS"}, // nominal width
Band{"Satellite and space", "GPS L5", 1'164'450'000, 1'188'450'000, 1'176'450'000, "raw", false, kRegionUSCA, BandKind::kChannel, BandColour::kSatellite, "IS-GPS-705", "GNSS"}, // nominal width
Band{"Satellite and space", "Galileo E1", 1'563'144'000, 1'587'696'000, 1'575'420'000, "raw", false, kRegionUSCA, BandKind::kChannel, BandColour::kSatellite, "Galileo OS SIS ICD", "GNSS"}, // nominal width
// GLONASS L1 FDMA channel centres 1602 + k*0.5625 MHz, k = -7..+6
Band{"Satellite and space", "GLONASS L1", 1'598'062'500, 1'605'375'000, 1'602'000'000, "raw", false, kRegionUSCA, BandKind::kChannel, BandColour::kSatellite, "GLONASS ICD", "GNSS"},
// ARISS: ISS downlinks
Band{"Satellite and space", "ISS voice", 145'787'500, 145'812'500, 145'800'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kSatellite, "ARISS", "ISS"}, // nominal width
Band{"Satellite and space", "ISS UHF", 437'787'500, 437'812'500, 437'800'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kSatellite, "ARISS", "ISS"}, // nominal width
// 47 CFR 2.106 US74, US246: passive radio astronomy, no transmissions authorised
Band{"Satellite and space", "Radio astro 1420", 1'400'000'000, 1'427'000'000, 1'420'405'800, "raw", false, kRegionUS, BandKind::kAllocation, BandColour::kOther, "47 CFR 2.106 US246", ""},
Band{"Satellite and space", "Hydrogen line", 1'419'405'800, 1'421'405'800, 1'420'405'800, "raw", false, kRegionUSCA, BandKind::kChannel, BandColour::kOther, "47 CFR 2.106 US246", "Radio astronomy"}, // nominal width
Band{"Satellite and space", "Radio astro 406", 406'100'000, 410'000'000, 408'000'000, "raw", false, kRegionUS, BandKind::kAllocation, BandColour::kOther, "47 CFR 2.106 US74", ""},
Band{"Satellite and space", "Radio astro ch 37", 608'000'000, 614'000'000, 611'000'000, "raw", false, kRegionUS, BandKind::kAllocation, BandColour::kOther, "47 CFR 2.106 US246", ""},
// NIST WWV (25 MHz experimental), WWVH, WWVB; NRC CHU is USB with carrier
Band{"Satellite and space", "WWV 2.5", 2'495'000, 2'505'000, 2'500'000, "am", false, kRegionUS, BandKind::kChannel, BandColour::kGovernment, "NIST", "Time signals"}, // nominal width
Band{"Satellite and space", "WWV 5", 4'995'000, 5'005'000, 5'000'000, "am", false, kRegionUS, BandKind::kChannel, BandColour::kGovernment, "NIST", "Time signals"}, // nominal width
Band{"Satellite and space", "WWV 10", 9'995'000, 10'005'000, 10'000'000, "am", false, kRegionUS, BandKind::kChannel, BandColour::kGovernment, "NIST", "Time signals"}, // nominal width
Band{"Satellite and space", "WWV 15", 14'995'000, 15'005'000, 15'000'000, "am", false, kRegionUS, BandKind::kChannel, BandColour::kGovernment, "NIST", "Time signals"}, // nominal width
Band{"Satellite and space", "WWV 20", 19'995'000, 20'005'000, 20'000'000, "am", false, kRegionUS, BandKind::kChannel, BandColour::kGovernment, "NIST", "Time signals"}, // nominal width
Band{"Satellite and space", "WWV 25", 24'995'000, 25'005'000, 25'000'000, "am", false, kRegionUS, BandKind::kChannel, BandColour::kGovernment, "NIST", "Time signals"}, // nominal width
Band{"Satellite and space", "WWVH 2.5", 2'495'000, 2'505'000, 2'500'000, "am", false, kRegionUS, BandKind::kChannel, BandColour::kGovernment, "NIST", "Time signals"}, // nominal width
Band{"Satellite and space", "WWVH 5", 4'995'000, 5'005'000, 5'000'000, "am", false, kRegionUS, BandKind::kChannel, BandColour::kGovernment, "NIST", "Time signals"}, // nominal width
Band{"Satellite and space", "WWVH 10", 9'995'000, 10'005'000, 10'000'000, "am", false, kRegionUS, BandKind::kChannel, BandColour::kGovernment, "NIST", "Time signals"}, // nominal width
Band{"Satellite and space", "WWVH 15", 14'995'000, 15'005'000, 15'000'000, "am", false, kRegionUS, BandKind::kChannel, BandColour::kGovernment, "NIST", "Time signals"}, // nominal width
Band{"Satellite and space", "WWVB", 59'950, 60'050, 60'000, "raw", false, kRegionUS, BandKind::kChannel, BandColour::kGovernment, "NIST", "Time signals"}, // nominal width
Band{"Satellite and space", "CHU 3.33", 3'330'000, 3'333'000, 3'330'000, "usb", false, kRegionCA, BandKind::kChannel, BandColour::kGovernment, "NRC Canada", "Time signals"}, // nominal width
Band{"Satellite and space", "CHU 7.85", 7'850'000, 7'853'000, 7'850'000, "usb", false, kRegionCA, BandKind::kChannel, BandColour::kGovernment, "NRC Canada", "Time signals"}, // nominal width
Band{"Satellite and space", "CHU 14.67", 14'670'000, 14'673'000, 14'670'000, "usb", false, kRegionCA, BandKind::kChannel, BandColour::kGovernment, "NRC Canada", "Time signals"}, // nominal width
