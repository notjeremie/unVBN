// Fixed identities of the unVBN WIC codec. Never change these: installers and the registry depend on them.
#pragma once

#include <guiddef.h>

// {09ADE08D-D804-4C88-A367-06D3E3152893}
static const CLSID CLSID_UnVBNDecoder = {0x09ade08d, 0xd804, 0x4c88, {0xa3, 0x67, 0x06, 0xd3, 0xe3, 0x15, 0x28, 0x93}};
// {D8750139-7878-4F08-8876-5DBCA6404DEB}
static const GUID GUID_ContainerFormatVBN = {0xd8750139, 0x7878, 0x4f08, {0x88, 0x76, 0x5d, 0xbc, 0xa6, 0x40, 0x4d, 0xeb}};
// {52ED2DBC-7B55-4539-988B-A2D11EE42A16}
static const GUID GUID_VendorUnVBN = {0x52ed2dbc, 0x7b55, 0x4539, {0x98, 0x8b, 0xa2, 0xd1, 0x1e, 0xe4, 0x2a, 0x16}};

#define UNVBN_DECODER_CLSID_STR L"{09ADE08D-D804-4C88-A367-06D3E3152893}"
#define UNVBN_CONTAINER_STR L"{D8750139-7878-4F08-8876-5DBCA6404DEB}"
#define UNVBN_VENDOR_STR L"{52ED2DBC-7B55-4539-988B-A2D11EE42A16}"
