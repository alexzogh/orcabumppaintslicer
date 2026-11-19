# QR Code generator library (C++) by Project Nayuki, MIT licensed.
# https://www.nayuki.io/page/qr-code-generator-library
#
# Vendored verbatim: the sources are kept exactly as upstream publishes them, warts and formatting
# included, so that a future update is a straight file copy rather than a merge. The MIT notice
# lives at the top of both files, which is what the licence asks for.
add_library(qrcodegen STATIC qrcodegen.cpp qrcodegen.hpp)
target_include_directories(qrcodegen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
