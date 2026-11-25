# Copyright (c) 2026 Airdraft
# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.20)
project(airdraft_rtl8852be LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "The experimental RTL8852BE radio requires MSVC on Windows.")
endif()

set(repo "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(winawdl "${repo}/third_party/winawdl")
set(orchard "${repo}/third_party/orchard-devourer")
foreach(dependency IN ITEMS "${winawdl}/radio/winawdl_radio.cpp" "${orchard}/CMakeLists.txt")
    if(NOT EXISTS "${dependency}")
        message(FATAL_ERROR "Missing ${dependency}. Fetch the RTL8852BE source dependencies first.")
    endif()
endforeach()

foreach(chip IN ITEMS JAGUAR1 8814 JAGUAR2_8822B JAGUAR2_8821C JAGUAR3_8822C
                      JAGUAR3_8822E 8733B MT7612U)
    set(DEVOURER_${chip} OFF CACHE BOOL "" FORCE)
endforeach()
# The pinned CMake file uses its 8852C option to add the shared Windows AX DMA
# plane. Retain that plane and remove the 8852C chip sources from the targets.
set(DEVOURER_KESTREL_8852C ON CACHE BOOL "" FORCE)
set(DEVOURER_KESTREL_8852B ON CACHE BOOL "" FORCE)
set(DEVOURER_PCIE ON CACHE BOOL "" FORCE)
set(DEVOURER_REGPROXY_INCLUDE_DIR "${repo}/driver/regproxy" CACHE PATH "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${orchard}" orchard-devourer EXCLUDE_FROM_ALL)

function(remove_8852c target source_pattern)
    get_target_property(sources "${target}" SOURCES)
    list(FILTER sources EXCLUDE REGEX "${source_pattern}")
    set_property(TARGET "${target}" PROPERTY SOURCES "${sources}")
    foreach(property IN ITEMS COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS)
        get_target_property(definitions "${target}" "${property}")
        if(definitions)
            list(FILTER definitions EXCLUDE REGEX "^(DEVOURER_HAVE_KESTREL_8852C|CONFIG_RTL8852C|BB_8852C_SUPPORT|RF_8852C_SUPPORT)(=|$)")
            set_property(TARGET "${target}" PROPERTY "${property}" "${definitions}")
        endif()
    endforeach()
endfunction()
remove_8852c(devourer "(^|/)hal8852c_fw[.][ch]$")
remove_8852c(kestrel_halbb "(^|/)halbb_8852c/")
remove_8852c(kestrel_halrf "(^|/)halrf_8852c/")

# Adapt the pinned sources in the build tree. Keep their original license headers.
function(replace_in_source variable old new)
    string(FIND "${${variable}}" "${old}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "The pinned ${variable} source changed. Review the RTL8852BE integration.")
    endif()
    string(REPLACE "${old}" "${new}" updated "${${variable}}")
    set(${variable} "${updated}" PARENT_SCOPE)
endfunction()

set(radio_source "${winawdl}/radio/winawdl_radio.cpp")
set(transport_source "${orchard}/src/PcieTransportWin.cpp")
set(dma_source "${orchard}/src/PcieTransport.cpp")
set(rings_source "${orchard}/src/PcieDmaAx.cpp")
set(factory_source "${orchard}/src/WiFiDriver.cpp")
set(kestrel_source "${orchard}/src/kestrel/RtlKestrelDevice.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${radio_source}" "${transport_source}" "${dma_source}"
    "${rings_source}" "${factory_source}" "${kestrel_source}")

file(READ "${radio_source}" radio)
replace_in_source(radio
    "bool stopping() { return g_devourer_should_stop; }"
    [=[std::atomic_bool supervisor_disconnected{false};
bool stopping() { return g_devourer_should_stop || supervisor_disconnected.load(); }]=])
replace_in_source(radio
    "  install_devourer_signal_handlers();"
    [=[  install_devourer_signal_handlers();
  if (GetEnvironmentVariableW(L"AIRDRAFT_SUPERVISED_RADIO", nullptr, 0) != 0) {
    std::thread([] {
      char command[128];
      while (std::fgets(command, sizeof(command), stdin)) {}
      supervisor_disconnected = true;
    }).detach();
  }]=])
replace_in_source(radio
    "  config.tx.retry_limit = options->retries;"
    "  config.tx.retry_limit = options->retries;\n  config.tuning.kestrel_be_pcie = true;")
replace_in_source(radio
    "#include \"block_ack.h\""
    "#include \"awdl_policy.h\"\n#include \"block_ack.h\"")
replace_in_source(radio
    [=[    std::vector<uint8_t> message;
    radio->StartRxLoop([&](const Packet &packet) {
      forward_received(packet, pipe, session, capture.get(), message);]=]
    [=[    std::vector<uint8_t> message;
    unsigned preferred_channel = 0, matching_frames = 0;
    radio->StartRxLoop([&](const Packet &packet) {
      // On a fresh install the service starts on channel 6. Follow a peer's
      // advertised 5 GHz channel once, before any transfer traffic arrives.
      if (channel == 6 && matching_frames < 3 && session.for_us == 0 &&
          !packet.RxAtrib.crc_err && awdl::is_awdl_action(packet.Data.data(), packet.Data.size())) {
        const auto fcs = packet.RxAtrib.fcs_present ? 4u : 0u;
        uint8_t sequence[16];
        if (awdl::peer_sequence(packet.Data.data(), packet.Data.size() - fcs, sequence)) {
          if (const auto observed = awdl::peer_channel(sequence)) {
            if (observed != preferred_channel) { preferred_channel = observed; matching_frames = 0; }
            if (++matching_frames == 3) {
              std::printf("radio: event: peer-channel %u\n", preferred_channel);
              std::fflush(stdout);
            }
          }
        }
      }
      forward_received(packet, pipe, session, capture.get(), message);]=])
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/winawdl_radio.cpp" CONTENT "${radio}" @ONLY)

file(READ "${transport_source}" transport)
replace_in_source(transport
    "  if (SetupDiEnumDeviceInterfaces(set, nullptr, iface, 0, &data)) {"
    "  for (DWORD index = 0; SetupDiEnumDeviceInterfaces(set, nullptr, iface, index, &data); ++index) {")
replace_in_source(transport
    [=[    std::vector<uint8_t> storage(size);]=]
    [=[    if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
    std::vector<uint8_t> storage(size);]=])
replace_in_source(transport
    [=[      _pci_vid = instance_id_field(instance_id, L"VEN_");]=]
    [=[      constexpr wchar_t b852[] = L"PCI\\VEN_10EC&DEV_B852";
      constexpr wchar_t b85b[] = L"PCI\\VEN_10EC&DEV_B85B";
      constexpr auto length = sizeof(b852) / sizeof(b852[0]) - 1;
      if ((_wcsnicmp(instance_id, b852, length) != 0 && _wcsnicmp(instance_id, b85b, length) != 0) ||
          (instance_id[length] != L'&' && instance_id[length] != L'\\' && instance_id[length] != L'\0'))
        continue;
      _pci_vid = instance_id_field(instance_id, L"VEN_");]=])
replace_in_source(transport
    [=[                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);]=]
    [=[                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (_os->proxy != INVALID_HANDLE_VALUE) break;]=])
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/PcieTransportWin.cpp" CONTENT "${transport}" @ONLY)

file(READ "${dma_source}" dma)
replace_in_source(dma
    "#if defined(DEVOURER_HAVE_KESTREL_8852C)\n#include \"PcieDmaAx.h\""
    "#if defined(DEVOURER_HAVE_KESTREL_8852B) || defined(DEVOURER_HAVE_KESTREL_8852C)\n#include \"PcieDmaAx.h\"")
replace_in_source(dma
    "  case kPciDidRtl8852be:"
    "  case kPciDidRtl8852be:\n  case 0xB85B:")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/PcieTransport.cpp" CONTENT "${dma}" @ONLY)

file(READ "${rings_source}" rings)
replace_in_source(rings
    "_be(t.pci_device_id() == 0xb852)"
    "_be(t.pci_device_id() == 0xb852 || t.pci_device_id() == 0xb85b)")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/PcieDmaAx.cpp" CONTENT "${rings}" @ONLY)

file(READ "${factory_source}" factory)
replace_in_source(factory
    "transport->pci_device_id())) {"
    "transport->pci_device_id() == 0xb85b ? 0xb852 : transport->pci_device_id())) {")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/WiFiDriver.cpp" CONTENT "${factory}" @ONLY)

file(READ "${kestrel_source}" kestrel)
replace_in_source(kestrel
    "out.pci_vid != 0x10ec || out.pci_did != 0xb852"
    "out.pci_vid != 0x10ec || (out.pci_did != 0xb852 && out.pci_did != 0xb85b)")
replace_in_source(kestrel
    "are not 10ec:b852 "
    "do not identify an RTL8852BE ")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/RtlKestrelDevice.cpp" CONTENT "${kestrel}" @ONLY)

get_target_property(devourer_sources devourer SOURCES)
list(FILTER devourer_sources EXCLUDE REGEX "(^|/)(PcieTransport(Win)?|PcieDmaAx|WiFiDriver|RtlKestrelDevice)[.]cpp$")
set_property(TARGET devourer PROPERTY SOURCES "${devourer_sources}")
target_include_directories(devourer PRIVATE "${orchard}/src/kestrel")
target_sources(devourer PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/PcieTransportWin.cpp" "${CMAKE_CURRENT_BINARY_DIR}/PcieTransport.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/PcieDmaAx.cpp" "${CMAKE_CURRENT_BINARY_DIR}/WiFiDriver.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/RtlKestrelDevice.cpp")

add_executable(airdraft-radio-rtl8852be "${CMAKE_CURRENT_BINARY_DIR}/winawdl_radio.cpp")
target_compile_features(airdraft-radio-rtl8852be PRIVATE cxx_std_20)
target_compile_definitions(airdraft-radio-rtl8852be PRIVATE _CRT_SECURE_NO_WARNINGS)
target_include_directories(airdraft-radio-rtl8852be PRIVATE "${winawdl}/radio" "${repo}/iwxradio/src")
target_link_libraries(airdraft-radio-rtl8852be PRIVATE devourer)
