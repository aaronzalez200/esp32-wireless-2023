# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "C:/Users/aaron/esp/esp-idf/components/bootloader/subproject"
  "C:/Users/aaron/esp32-dev/esp32-eti/ETI-Wireless-Module/build/bootloader"
  "C:/Users/aaron/esp32-dev/esp32-eti/ETI-Wireless-Module/build/bootloader-prefix"
  "C:/Users/aaron/esp32-dev/esp32-eti/ETI-Wireless-Module/build/bootloader-prefix/tmp"
  "C:/Users/aaron/esp32-dev/esp32-eti/ETI-Wireless-Module/build/bootloader-prefix/src/bootloader-stamp"
  "C:/Users/aaron/esp32-dev/esp32-eti/ETI-Wireless-Module/build/bootloader-prefix/src"
  "C:/Users/aaron/esp32-dev/esp32-eti/ETI-Wireless-Module/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/aaron/esp32-dev/esp32-eti/ETI-Wireless-Module/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/aaron/esp32-dev/esp32-eti/ETI-Wireless-Module/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
