// VHF low: first and last channels below 50 MHz in 47 CFR 90.35(b)(3) are 25.02 and 49.58 MHz;
// 47 CFR 90.20(c)(3) public safety channels run 30.86-46.58 MHz
Band{"Land mobile", "VHF low", 25'020'000, 49'580'000, 40'000'000, "nfm", false, kRegionUS, BandKind::kAllocation, BandColour::kLandMobile, "47 CFR 90.35", ""},
Band{"Public safety", "VHF low PS", 30'860'000, 46'580'000, 39'000'000, "nfm", false, kRegionUS, BandKind::kSubBand, BandColour::kPublicSafety, "47 CFR 90.20", ""},
// 47 CFR 90.20, 90.35; federal segment 162.0125-173.2 MHz per 47 CFR 2.106 and the NTIA Manual
Band{"Land mobile", "VHF high", 150'800'000, 174'000'000, 155'000'000, "nfm", false, kRegionUS, BandKind::kAllocation, BandColour::kLandMobile, "47 CFR 90.20, 90.35", ""},
Band{"Land mobile", "VHF federal", 162'012'500, 173'200'000, 165'000'000, "nfm", false, kRegionUS, BandKind::kSubBand, BandColour::kGovernment, "47 CFR 2.106; NTIA Manual 4.3.7", ""},
// 47 CFR 90.715: base 220-221 MHz, mobile 221-222 MHz
Band{"Land mobile", "220 MHz", 220'000'000, 222'000'000, 221'000'000, "nfm", false, kRegionUS, BandKind::kAllocation, BandColour::kLandMobile, "47 CFR 90.715", ""},
Band{"Land mobile", "UHF 450-470", 450'000'000, 470'000'000, 460'000'000, "nfm", false, kRegionUS, BandKind::kAllocation, BandColour::kLandMobile, "47 CFR 90.20, 90.35", ""},
Band{"Land mobile", "460-464 MHz", 460'000'000, 464'000'000, 462'000'000, "nfm", false, kRegionUS, BandKind::kSubBand, BandColour::kLandMobile, "47 CFR 90.20, 90.35", ""},
// 47 CFR 90.303: T-band only in Boston, Chicago, Cleveland, Dallas/Fort Worth, Detroit, Houston,
// Los Angeles, Miami, New York/NE New Jersey, Philadelphia, Pittsburgh, San Francisco/Oakland, Washington DC
Band{"Land mobile", "T-band", 470'000'000, 512'000'000, 491'000'000, "nfm", false, kRegionUS, BandKind::kAllocation, BandColour::kLandMobile, "47 CFR 90.303", ""},
// 47 CFR 90.531: 700 MHz public safety; broadband is FirstNet Band 14
Band{"Public safety", "700 PS low", 758'000'000, 775'000'000, 770'000'000, "p25p1", false, kRegionUS, BandKind::kAllocation, BandColour::kPublicSafety, "47 CFR 90.531", ""},
Band{"Public safety", "700 PS high", 788'000'000, 805'000'000, 800'000'000, "p25p1", false, kRegionUS, BandKind::kAllocation, BandColour::kPublicSafety, "47 CFR 90.531", ""},
Band{"Public safety", "Band 14 down", 758'000'000, 768'000'000, 763'000'000, "raw", false, kRegionUS, BandKind::kSubBand, BandColour::kPublicSafety, "47 CFR 90.531", ""},
Band{"Public safety", "Band 14 up", 788'000'000, 798'000'000, 793'000'000, "raw", false, kRegionUS, BandKind::kSubBand, BandColour::kPublicSafety, "47 CFR 90.531", ""},
Band{"Public safety", "700 guard", 768'000'000, 769'000'000, 768'500'000, "", false, kRegionUS, BandKind::kSubBand, BandColour::kPublicSafety, "47 CFR 90.531", ""},
Band{"Public safety", "700 guard up", 798'000'000, 799'000'000, 798'500'000, "", false, kRegionUS, BandKind::kSubBand, BandColour::kPublicSafety, "47 CFR 90.531", ""},
Band{"Public safety", "700 NB base", 769'000'000, 775'000'000, 772'000'000, "p25p1", false, kRegionUS, BandKind::kSubBand, BandColour::kPublicSafety, "47 CFR 90.531", ""},
Band{"Public safety", "700 NB mobile", 799'000'000, 805'000'000, 802'000'000, "p25p1", false, kRegionUS, BandKind::kSubBand, BandColour::kPublicSafety, "47 CFR 90.531", ""},
// ISED SRSP-511: Canadian 700 MHz land mobile at 768-776/798-806 MHz
Band{"Public safety", "700 LMR base", 768'000'000, 776'000'000, 772'000'000, "p25p1", false, kRegionCA, BandKind::kAllocation, BandColour::kPublicSafety, "ISED SRSP-511", ""},
Band{"Public safety", "700 LMR mobile", 798'000'000, 806'000'000, 802'000'000, "p25p1", false, kRegionCA, BandKind::kAllocation, BandColour::kPublicSafety, "ISED SRSP-511", ""},
// 800 MHz after rebanding; segment edges from FCC Public Notice DA 16-1362
Band{"Land mobile", "800 MHz mobile", 806'000'000, 824'000'000, 815'000'000, "nfm", false, kRegionUSCA, BandKind::kAllocation, BandColour::kLandMobile, "47 CFR 90.613; ISED SRSP-502", ""},
Band{"Land mobile", "800 MHz base", 851'000'000, 869'000'000, 860'000'000, "p25p1", false, kRegionUSCA, BandKind::kAllocation, BandColour::kLandMobile, "47 CFR 90.613; ISED SRSP-502", ""},
Band{"Public safety", "NPSPAC mobile", 806'000'000, 809'000'000, 807'500'000, "nfm", false, kRegionUS, BandKind::kSubBand, BandColour::kPublicSafety, "FCC DA 16-1362", ""},
Band{"Public safety", "NPSPAC base", 851'000'000, 854'000'000, 852'500'000, "nfm", false, kRegionUS, BandKind::kSubBand, BandColour::kPublicSafety, "FCC DA 16-1362", ""},
Band{"Land mobile", "800 interleaved mobile", 809'000'000, 815'000'000, 812'000'000, "nfm", false, kRegionUS, BandKind::kSubBand, BandColour::kLandMobile, "FCC DA 16-1362", ""},
Band{"Land mobile", "800 interleaved base", 854'000'000, 860'000'000, 857'000'000, "p25p1", false, kRegionUS, BandKind::kSubBand, BandColour::kLandMobile, "FCC DA 16-1362", ""},
Band{"Land mobile", "800 expansion mobile", 815'000'000, 816'000'000, 815'500'000, "nfm", false, kRegionUS, BandKind::kSubBand, BandColour::kLandMobile, "FCC DA 16-1362", ""},
Band{"Land mobile", "800 expansion base", 860'000'000, 861'000'000, 860'500'000, "nfm", false, kRegionUS, BandKind::kSubBand, BandColour::kLandMobile, "FCC DA 16-1362", ""},
Band{"Land mobile", "800 guard mobile", 816'000'000, 817'000'000, 816'500'000, "nfm", false, kRegionUS, BandKind::kSubBand, BandColour::kLandMobile, "FCC DA 16-1362", ""},
Band{"Land mobile", "800 guard base", 861'000'000, 862'000'000, 861'500'000, "nfm", false, kRegionUS, BandKind::kSubBand, BandColour::kLandMobile, "FCC DA 16-1362", ""},
Band{"Land mobile", "ESMR mobile", 817'000'000, 824'000'000, 820'500'000, "raw", false, kRegionUS, BandKind::kSubBand, BandColour::kLandMobile, "FCC DA 16-1362", ""},
Band{"Land mobile", "ESMR base", 862'000'000, 869'000'000, 865'500'000, "raw", false, kRegionUS, BandKind::kSubBand, BandColour::kLandMobile, "FCC DA 16-1362", ""},
// ISED SRSP-502: Canada was not rebanded; public safety stays at 821-824/866-869 MHz
Band{"Public safety", "800 PS mobile", 821'000'000, 824'000'000, 822'500'000, "nfm", false, kRegionCA, BandKind::kSubBand, BandColour::kPublicSafety, "ISED SRSP-502", ""},
Band{"Public safety", "800 PS base", 866'000'000, 869'000'000, 867'500'000, "p25p1", false, kRegionCA, BandKind::kSubBand, BandColour::kPublicSafety, "ISED SRSP-502", ""},
// 896-901/935-940 MHz; 3/3 broadband segment per FCC 17-108, whole band broadband-eligible per FCC 26-9
Band{"Land mobile", "900 MHz mobile", 896'000'000, 901'000'000, 898'500'000, "nfm", false, kRegionUS, BandKind::kAllocation, BandColour::kLandMobile, "FCC 26-9", ""},
Band{"Land mobile", "900 MHz base", 935'000'000, 940'000'000, 937'500'000, "nfm", false, kRegionUS, BandKind::kAllocation, BandColour::kLandMobile, "FCC 26-9", ""},
Band{"Land mobile", "900 broadband up", 897'500'000, 900'500'000, 899'000'000, "raw", false, kRegionUS, BandKind::kSubBand, BandColour::kLandMobile, "FCC 17-108", ""},
Band{"Land mobile", "900 broadband down", 936'500'000, 939'500'000, 938'000'000, "raw", false, kRegionUS, BandKind::kSubBand, BandColour::kLandMobile, "FCC 17-108", ""},
Band{"Public safety", "4.9 GHz", 4'940'000'000, 4'990'000'000, 4'965'000'000, "raw", false, kRegionUS, BandKind::kAllocation, BandColour::kPublicSafety, "47 CFR 90.1203", ""},
// Paging: 47 CFR 22.531 channels are 20 kHz wide; 47 CFR 90.494 private paging
Band{"Land mobile", "Paging 152.24", 152'230'000, 152'250'000, 152'240'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 22.531", "Paging"},
Band{"Land mobile", "Paging 152.84", 152'830'000, 152'850'000, 152'840'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 22.531", "Paging"},
Band{"Land mobile", "Paging 158.10", 158'090'000, 158'110'000, 158'100'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 22.531", "Paging"},
Band{"Land mobile", "Paging 158.70", 158'690'000, 158'710'000, 158'700'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 22.531", "Paging"},
Band{"Land mobile", "Paging 929", 929'000'000, 930'000'000, 929'500'000, "nfm", false, kRegionUS, BandKind::kAllocation, BandColour::kLandMobile, "47 CFR 90.494", ""},
Band{"Land mobile", "Paging 931", 931'000'000, 932'000'000, 931'500'000, "nfm", false, kRegionUS, BandKind::kAllocation, BandColour::kLandMobile, "47 CFR 22.531", ""},
// NIFOG national interoperability channels, base/output frequency
Band{"Public safety", "VCALL10", 155'740'000, 155'765'000, 155'752'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "VTAC11", 151'125'000, 151'150'000, 151'137'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "VTAC12", 154'440'000, 154'465'000, 154'452'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "VTAC13", 158'725'000, 158'750'000, 158'737'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "VTAC14", 159'460'000, 159'485'000, 159'472'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "VFIRE21", 154'267'500, 154'292'500, 154'280'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "VMED28", 155'327'500, 155'352'500, 155'340'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "VLAW31", 155'462'500, 155'487'500, 155'475'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "UCALL40", 453'200'000, 453'225'000, 453'212'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "UTAC41", 453'450'000, 453'475'000, 453'462'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "UTAC42", 453'700'000, 453'725'000, 453'712'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "UTAC43", 453'850'000, 453'875'000, 453'862'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "7CALL50", 769'231'250, 769'256'250, 769'243'750, "p25p1", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "7TAC51", 769'131'250, 769'156'250, 769'143'750, "p25p1", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "7TAC52", 769'631'250, 769'656'250, 769'643'750, "p25p1", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "7TAC53", 770'131'250, 770'156'250, 770'143'750, "p25p1", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "7TAC54", 770'631'250, 770'656'250, 770'643'750, "p25p1", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "7TAC55", 769'731'250, 769'756'250, 769'743'750, "p25p1", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "7TAC56", 770'231'250, 770'256'250, 770'243'750, "p25p1", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "8CALL90", 851'000'000, 851'025'000, 851'012'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "8TAC91", 851'500'000, 851'525'000, 851'512'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "8TAC92", 852'000'000, 852'025'000, 852'012'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "8TAC93", 852'500'000, 852'525'000, 852'512'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
Band{"Public safety", "8TAC94", 853'000'000, 853'025'000, 853'012'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kPublicSafety, "NIFOG", "Interop channels"}, // nominal width
// Railroad: AAR channels 07-97, 160.215-161.565 MHz at 15 kHz spacing
Band{"Land mobile", "Railroad", 160'207'500, 161'572'500, 160'890'000, "nfm", false, kRegionUSCA, BandKind::kAllocation, BandColour::kLandMobile, "47 CFR 90.35; AAR", ""},
Band{"Land mobile", "AAR 07", 160'207'500, 160'222'500, 160'215'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 08", 160'222'500, 160'237'500, 160'230'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 09", 160'237'500, 160'252'500, 160'245'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 10", 160'252'500, 160'267'500, 160'260'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 11", 160'267'500, 160'282'500, 160'275'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 12", 160'282'500, 160'297'500, 160'290'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 13", 160'297'500, 160'312'500, 160'305'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 14", 160'312'500, 160'327'500, 160'320'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 15", 160'327'500, 160'342'500, 160'335'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 16", 160'342'500, 160'357'500, 160'350'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 17", 160'357'500, 160'372'500, 160'365'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 18", 160'372'500, 160'387'500, 160'380'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 19", 160'387'500, 160'402'500, 160'395'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 20", 160'402'500, 160'417'500, 160'410'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 21", 160'417'500, 160'432'500, 160'425'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 22", 160'432'500, 160'447'500, 160'440'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 23", 160'447'500, 160'462'500, 160'455'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 24", 160'462'500, 160'477'500, 160'470'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 25", 160'477'500, 160'492'500, 160'485'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 26", 160'492'500, 160'507'500, 160'500'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 27", 160'507'500, 160'522'500, 160'515'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 28", 160'522'500, 160'537'500, 160'530'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 29", 160'537'500, 160'552'500, 160'545'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 30", 160'552'500, 160'567'500, 160'560'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 31", 160'567'500, 160'582'500, 160'575'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 32", 160'582'500, 160'597'500, 160'590'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 33", 160'597'500, 160'612'500, 160'605'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 34", 160'612'500, 160'627'500, 160'620'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 35", 160'627'500, 160'642'500, 160'635'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 36", 160'642'500, 160'657'500, 160'650'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 37", 160'657'500, 160'672'500, 160'665'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 38", 160'672'500, 160'687'500, 160'680'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 39", 160'687'500, 160'702'500, 160'695'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 40", 160'702'500, 160'717'500, 160'710'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 41", 160'717'500, 160'732'500, 160'725'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 42", 160'732'500, 160'747'500, 160'740'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 43", 160'747'500, 160'762'500, 160'755'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 44", 160'762'500, 160'777'500, 160'770'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 45", 160'777'500, 160'792'500, 160'785'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 46", 160'792'500, 160'807'500, 160'800'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 47", 160'807'500, 160'822'500, 160'815'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 48", 160'822'500, 160'837'500, 160'830'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 49", 160'837'500, 160'852'500, 160'845'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 50", 160'852'500, 160'867'500, 160'860'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 51", 160'867'500, 160'882'500, 160'875'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 52", 160'882'500, 160'897'500, 160'890'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 53", 160'897'500, 160'912'500, 160'905'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 54", 160'912'500, 160'927'500, 160'920'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 55", 160'927'500, 160'942'500, 160'935'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 56", 160'942'500, 160'957'500, 160'950'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 57", 160'957'500, 160'972'500, 160'965'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 58", 160'972'500, 160'987'500, 160'980'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 59", 160'987'500, 161'002'500, 160'995'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 60", 161'002'500, 161'017'500, 161'010'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 61", 161'017'500, 161'032'500, 161'025'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 62", 161'032'500, 161'047'500, 161'040'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 63", 161'047'500, 161'062'500, 161'055'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 64", 161'062'500, 161'077'500, 161'070'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 65", 161'077'500, 161'092'500, 161'085'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 66", 161'092'500, 161'107'500, 161'100'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 67", 161'107'500, 161'122'500, 161'115'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 68", 161'122'500, 161'137'500, 161'130'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 69", 161'137'500, 161'152'500, 161'145'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 70", 161'152'500, 161'167'500, 161'160'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 71", 161'167'500, 161'182'500, 161'175'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 72", 161'182'500, 161'197'500, 161'190'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 73", 161'197'500, 161'212'500, 161'205'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 74", 161'212'500, 161'227'500, 161'220'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 75", 161'227'500, 161'242'500, 161'235'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 76", 161'242'500, 161'257'500, 161'250'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 77", 161'257'500, 161'272'500, 161'265'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 78", 161'272'500, 161'287'500, 161'280'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 79", 161'287'500, 161'302'500, 161'295'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 80", 161'302'500, 161'317'500, 161'310'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 81", 161'317'500, 161'332'500, 161'325'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 82", 161'332'500, 161'347'500, 161'340'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 83", 161'347'500, 161'362'500, 161'355'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 84", 161'362'500, 161'377'500, 161'370'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 85", 161'377'500, 161'392'500, 161'385'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 86", 161'392'500, 161'407'500, 161'400'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 87", 161'407'500, 161'422'500, 161'415'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 88", 161'422'500, 161'437'500, 161'430'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 89", 161'437'500, 161'452'500, 161'445'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 90", 161'452'500, 161'467'500, 161'460'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 91", 161'467'500, 161'482'500, 161'475'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 92", 161'482'500, 161'497'500, 161'490'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 93", 161'497'500, 161'512'500, 161'505'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 94", 161'512'500, 161'527'500, 161'520'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 95", 161'527'500, 161'542'500, 161'535'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 96", 161'542'500, 161'557'500, 161'550'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
Band{"Land mobile", "AAR 97", 161'557'500, 161'572'500, 161'565'000, "nfm", false, kRegionUSCA, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35; AAR", "Railroad AAR"},
// Business itinerant colour-dot channels, 47 CFR 90.35; blue and green dot are MURS, 47 CFR 95.2763
Band{"Land mobile", "Red dot", 151'612'500, 151'637'500, 151'625'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35", "Business itinerant"}, // nominal width
Band{"Land mobile", "Purple dot", 151'942'500, 151'967'500, 151'955'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35", "Business itinerant"}, // nominal width
Band{"Land mobile", "Blue dot", 154'557'500, 154'582'500, 154'570'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 95.2763", "Business itinerant"}, // nominal width
Band{"Land mobile", "Green dot", 154'587'500, 154'612'500, 154'600'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 95.2763", "Business itinerant"}, // nominal width
Band{"Land mobile", "Brown dot", 464'487'500, 464'512'500, 464'500'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35", "Business itinerant"}, // nominal width
Band{"Land mobile", "Yellow dot", 464'537'500, 464'562'500, 464'550'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35", "Business itinerant"}, // nominal width
Band{"Land mobile", "J dot", 467'750'000, 467'775'000, 467'762'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35", "Business itinerant"}, // nominal width
Band{"Land mobile", "K dot", 467'800'000, 467'825'000, 467'812'500, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35", "Business itinerant"}, // nominal width
Band{"Land mobile", "Silver star", 467'837'500, 467'862'500, 467'850'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35", "Business itinerant"}, // nominal width
Band{"Land mobile", "Gold star", 467'862'500, 467'887'500, 467'875'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35", "Business itinerant"}, // nominal width
Band{"Land mobile", "Red star", 467'887'500, 467'912'500, 467'900'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35", "Business itinerant"}, // nominal width
Band{"Land mobile", "Blue star", 467'912'500, 467'937'500, 467'925'000, "nfm", false, kRegionUS, BandKind::kChannel, BandColour::kLandMobile, "47 CFR 90.35", "Business itinerant"}, // nominal width
