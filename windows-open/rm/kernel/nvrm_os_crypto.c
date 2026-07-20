#include "nvplatform_crypto_rm.h"
#include "nvplatform_time_rm.h"

#include <stddef.h>

typedef struct WINDOWS_RM_X509_DATE_TIME
{
    NvU64 sortableValue;
} WINDOWS_RM_X509_DATE_TIME;

static NvBool parseDateTimeField(
    const char* text,
    NvU32 offset,
    NvU32 length,
    NvU32* value)
{
    NvU32 result = 0;
    for (NvU32 index = 0; index < length; ++index)
    {
        const char character = text[offset + index];
        if (character < '0' || character > '9')
        {
            return NV_FALSE;
        }
        result = result * 10 + (NvU32)(character - '0');
    }

    *value = result;
    return NV_TRUE;
}

static NvBool isLeapYear(NvU32 year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

NV_STATUS osGetRandomBytes(NvU8* bytes, NvU16 byteCount)
{
    return NvPlatformRmGetRandomBytes(bytes, byteCount);
}

NvBool libspdm_random_bytes(NvU8* output, size_t size)
{
    if (size > NV_U32_MAX)
    {
        return NV_FALSE;
    }

    return NvPlatformRmGetRandomBytes(output, (NvU32)size) == NV_OK;
}

NvBool libspdm_check_crypto_backend(void)
{
    return NV_TRUE;
}

NvBool libspdm_x509_set_date_time(
    const char* dateTimeString,
    void* dateTime,
    size_t* dateTimeSize)
{
    static const NvU8 daysPerMonth[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    const size_t requiredSize = sizeof(WINDOWS_RM_X509_DATE_TIME);
    NvU32 year;
    NvU32 month;
    NvU32 day;
    NvU32 hour;
    NvU32 minute;
    NvU32 second;

    if (dateTimeString == NULL || dateTimeSize == NULL)
    {
        return NV_FALSE;
    }
    for (NvU32 index = 0; index < 15; ++index)
    {
        if (dateTimeString[index] == '\0')
        {
            return NV_FALSE;
        }
    }
    if (dateTimeString[14] != 'Z' || dateTimeString[15] != '\0' ||
        !parseDateTimeField(dateTimeString, 0, 4, &year) ||
        !parseDateTimeField(dateTimeString, 4, 2, &month) ||
        !parseDateTimeField(dateTimeString, 6, 2, &day) ||
        !parseDateTimeField(dateTimeString, 8, 2, &hour) ||
        !parseDateTimeField(dateTimeString, 10, 2, &minute) ||
        !parseDateTimeField(dateTimeString, 12, 2, &second))
    {
        return NV_FALSE;
    }

    NvU32 maximumDay = month >= 1 && month <= 12
        ? daysPerMonth[month - 1]
        : 0;
    if (month == 2 && isLeapYear(year))
    {
        ++maximumDay;
    }
    if (year == 0 || day == 0 || day > maximumDay ||
        hour > 23 || minute > 59 || second > 59)
    {
        return NV_FALSE;
    }

    if (dateTime == NULL || *dateTimeSize < requiredSize)
    {
        *dateTimeSize = requiredSize;
        return NV_FALSE;
    }

    WINDOWS_RM_X509_DATE_TIME* value = dateTime;
    value->sortableValue = year;
    value->sortableValue = value->sortableValue * 100 + month;
    value->sortableValue = value->sortableValue * 100 + day;
    value->sortableValue = value->sortableValue * 100 + hour;
    value->sortableValue = value->sortableValue * 100 + minute;
    value->sortableValue = value->sortableValue * 100 + second;
    *dateTimeSize = requiredSize;
    return NV_TRUE;
}

NvS32 libspdm_x509_compare_date_time(
    const void* firstDateTime,
    const void* secondDateTime)
{
    if (firstDateTime == NULL || secondDateTime == NULL)
    {
        return -2;
    }

    const WINDOWS_RM_X509_DATE_TIME* first = firstDateTime;
    const WINDOWS_RM_X509_DATE_TIME* second = secondDateTime;
    if (first->sortableValue == second->sortableValue)
    {
        return 0;
    }
    return first->sortableValue > second->sortableValue ? 1 : -1;
}

void libspdm_sleep(NvU64 microseconds)
{
    while (microseconds != 0)
    {
        const NvU32 delay = microseconds > NV_U32_MAX
            ? NV_U32_MAX
            : (NvU32)microseconds;
        if (NvPlatformDelayRmMicroseconds(delay) != NV_OK)
        {
            return;
        }
        microseconds -= delay;
    }
}

NvBool libspdm_encode_base64(
    const NvU8* source,
    NvU8* destination,
    size_t sourceLength,
    size_t* destinationLength)
{
    static const NvU8 alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    const size_t maximumSize = (size_t)-1;
    if (destinationLength == NULL ||
        (source == NULL && sourceLength != 0) ||
        sourceLength > maximumSize - 2)
    {
        return NV_FALSE;
    }

    const size_t groupCount = (sourceLength + 2) / 3;
    if (groupCount > maximumSize / 4)
    {
        return NV_FALSE;
    }
    const size_t requiredLength = groupCount * 4;
    if (*destinationLength < requiredLength)
    {
        *destinationLength = requiredLength;
        return NV_FALSE;
    }
    if (destination == NULL && requiredLength != 0)
    {
        return NV_FALSE;
    }

    size_t sourceIndex = 0;
    size_t destinationIndex = 0;
    while (sourceLength - sourceIndex >= 3)
    {
        const NvU32 value = ((NvU32)source[sourceIndex] << 16) |
            ((NvU32)source[sourceIndex + 1] << 8) |
            source[sourceIndex + 2];
        destination[destinationIndex++] = alphabet[(value >> 18) & 0x3f];
        destination[destinationIndex++] = alphabet[(value >> 12) & 0x3f];
        destination[destinationIndex++] = alphabet[(value >> 6) & 0x3f];
        destination[destinationIndex++] = alphabet[value & 0x3f];
        sourceIndex += 3;
    }

    const size_t remaining = sourceLength - sourceIndex;
    if (remaining != 0)
    {
        NvU32 value = (NvU32)source[sourceIndex] << 16;
        if (remaining == 2)
        {
            value |= (NvU32)source[sourceIndex + 1] << 8;
        }
        destination[destinationIndex++] = alphabet[(value >> 18) & 0x3f];
        destination[destinationIndex++] = alphabet[(value >> 12) & 0x3f];
        destination[destinationIndex++] = remaining == 2
            ? alphabet[(value >> 6) & 0x3f]
            : '=';
        destination[destinationIndex++] = '=';
    }

    *destinationLength = destinationIndex;
    return NV_TRUE;
}

NvBool libspdm_asn1_get_tag(
    NvU8** pointer,
    const NvU8* end,
    size_t* length,
    NvU32 tag)
{
    if (pointer == NULL || *pointer == NULL || end == NULL || length == NULL ||
        tag > NV_U8_MAX || *pointer >= end || **pointer != (NvU8)tag)
    {
        return NV_FALSE;
    }

    const NvU8* cursor = *pointer + 1;
    if (cursor >= end)
    {
        return NV_FALSE;
    }

    size_t contentLength = 0;
    size_t headerLength = 2;
    const NvU8 firstLength = *cursor++;
    if ((firstLength & 0x80) == 0)
    {
        contentLength = firstLength;
    }
    else
    {
        const NvU8 lengthBytes = firstLength & 0x7f;
        if (lengthBytes == 0 || lengthBytes > sizeof(size_t) ||
            (size_t)(end - cursor) < lengthBytes || cursor[0] == 0)
        {
            return NV_FALSE;
        }

        headerLength += lengthBytes;
        for (NvU8 index = 0; index < lengthBytes; ++index)
        {
            if (contentLength > ((size_t)-1) >> 8)
            {
                return NV_FALSE;
            }
            contentLength = (contentLength << 8) | cursor[index];
        }
        if (contentLength < 0x80)
        {
            return NV_FALSE;
        }
    }

    if (contentLength > (size_t)(end - *pointer) - headerLength)
    {
        return NV_FALSE;
    }
    *length = headerLength + contentLength;
    return NV_TRUE;
}

static NvBool getDerElement(
    const NvU8* element,
    const NvU8* end,
    NvU8 tag,
    const NvU8** content,
    size_t* contentLength,
    size_t* elementLength)
{
    NvU8* mutableElement = (NvU8*)element;
    size_t length;

    if (content == NULL || contentLength == NULL || elementLength == NULL ||
        !libspdm_asn1_get_tag(&mutableElement, end, &length, tag))
    {
        return NV_FALSE;
    }

    const size_t headerLength = element[1] < 0x80
        ? 2
        : 2 + (element[1] & 0x7f);
    if (headerLength > length)
    {
        return NV_FALSE;
    }

    *content = element + headerLength;
    *contentLength = length - headerLength;
    *elementLength = length;
    return NV_TRUE;
}

static NvBool getCertificateTbsFields(
    const NvU8* certificate,
    size_t certificateLength,
    const NvU8** firstField,
    const NvU8** fieldsEnd)
{
    if (certificate == NULL || certificateLength == 0 ||
        firstField == NULL || fieldsEnd == NULL)
    {
        return NV_FALSE;
    }

    const NvU8* certificateEnd = certificate + certificateLength;
    const NvU8* certificateContent;
    size_t certificateContentLength;
    size_t certificateElementLength;
    if (!getDerElement(
        certificate,
        certificateEnd,
        0x30,
        &certificateContent,
        &certificateContentLength,
        &certificateElementLength) ||
        certificateElementLength != certificateLength)
    {
        return NV_FALSE;
    }

    const NvU8* tbsContent;
    size_t tbsContentLength;
    size_t tbsElementLength;
    if (!getDerElement(
        certificateContent,
        certificateEnd,
        0x30,
        &tbsContent,
        &tbsContentLength,
        &tbsElementLength))
    {
        return NV_FALSE;
    }

    const NvU8* signatureAlgorithm = certificateContent + tbsElementLength;
    const NvU8* ignoredContent;
    size_t ignoredContentLength;
    size_t signatureAlgorithmLength;
    if (!getDerElement(
        signatureAlgorithm,
        certificateEnd,
        0x30,
        &ignoredContent,
        &ignoredContentLength,
        &signatureAlgorithmLength))
    {
        return NV_FALSE;
    }

    const NvU8* signature = signatureAlgorithm + signatureAlgorithmLength;
    const NvU8* signatureContent;
    size_t signatureContentLength;
    size_t signatureLength;
    if (!getDerElement(
        signature,
        certificateEnd,
        0x03,
        &signatureContent,
        &signatureContentLength,
        &signatureLength) ||
        signature + signatureLength != certificateEnd ||
        signatureContentLength == 0 || signatureContent[0] > 7)
    {
        return NV_FALSE;
    }

    *firstField = tbsContent;
    *fieldsEnd = tbsContent + tbsContentLength;
    return NV_TRUE;
}

NvBool libspdm_x509_get_version(
    const NvU8* certificate,
    size_t certificateLength,
    size_t* version)
{
    const NvU8* field;
    const NvU8* fieldsEnd;
    if (version == NULL ||
        !getCertificateTbsFields(
            certificate,
            certificateLength,
            &field,
            &fieldsEnd))
    {
        return NV_FALSE;
    }

    if (field == fieldsEnd || *field != 0xa0)
    {
        if (field == fieldsEnd || *field != 0x02)
        {
            return NV_FALSE;
        }
        *version = 0;
        return NV_TRUE;
    }

    const NvU8* explicitContent;
    size_t explicitContentLength;
    size_t explicitElementLength;
    if (!getDerElement(
        field,
        fieldsEnd,
        0xa0,
        &explicitContent,
        &explicitContentLength,
        &explicitElementLength))
    {
        return NV_FALSE;
    }

    const NvU8* integerContent;
    size_t integerContentLength;
    size_t integerElementLength;
    if (!getDerElement(
        explicitContent,
        explicitContent + explicitContentLength,
        0x02,
        &integerContent,
        &integerContentLength,
        &integerElementLength) ||
        integerElementLength != explicitContentLength ||
        integerContentLength == 0 || (integerContent[0] & 0x80) != 0 ||
        (integerContentLength > 1 && integerContent[0] == 0 &&
         (integerContent[1] & 0x80) == 0))
    {
        return NV_FALSE;
    }

    size_t value = 0;
    for (size_t index = 0; index < integerContentLength; ++index)
    {
        if (value > ((size_t)-1) >> 8)
        {
            return NV_FALSE;
        }
        value = (value << 8) | integerContent[index];
    }

    const NvU8* serialField = field + explicitElementLength;
    if (serialField == fieldsEnd || *serialField != 0x02)
    {
        return NV_FALSE;
    }

    *version = value;
    return NV_TRUE;
}

NvBool libspdm_x509_get_serial_number(
    const NvU8* certificate,
    size_t certificateLength,
    NvU8* serialNumber,
    size_t* serialNumberLength)
{
    const NvU8* field;
    const NvU8* fieldsEnd;
    if (serialNumberLength == NULL ||
        !getCertificateTbsFields(
            certificate,
            certificateLength,
            &field,
            &fieldsEnd))
    {
        return NV_FALSE;
    }

    size_t ignoredVersion;
    if (!libspdm_x509_get_version(
        certificate,
        certificateLength,
        &ignoredVersion))
    {
        return NV_FALSE;
    }

    if (field < fieldsEnd && *field == 0xa0)
    {
        const NvU8* ignoredContent;
        size_t ignoredContentLength;
        size_t versionElementLength;
        if (!getDerElement(
            field,
            fieldsEnd,
            0xa0,
            &ignoredContent,
            &ignoredContentLength,
            &versionElementLength))
        {
            return NV_FALSE;
        }
        field += versionElementLength;
    }

    const NvU8* integerContent;
    size_t integerContentLength;
    size_t integerElementLength;
    if (!getDerElement(
        field,
        fieldsEnd,
        0x02,
        &integerContent,
        &integerContentLength,
        &integerElementLength) ||
        integerContentLength == 0 || (integerContent[0] & 0x80) != 0 ||
        (integerContentLength > 1 && integerContent[0] == 0 &&
         (integerContent[1] & 0x80) == 0))
    {
        return NV_FALSE;
    }

    if (serialNumber == NULL || *serialNumberLength < integerContentLength)
    {
        *serialNumberLength = integerContentLength;
        return NV_FALSE;
    }

    for (size_t index = 0; index < integerContentLength; ++index)
    {
        serialNumber[index] = integerContent[index];
    }
    *serialNumberLength = integerContentLength;
    return NV_TRUE;
}

static NvBool validateDerName(
    const NvU8* name,
    const NvU8* end)
{
    const NvU8* nameContent;
    size_t nameContentLength;
    size_t nameLength;
    if (!getDerElement(
        name,
        end,
        0x30,
        &nameContent,
        &nameContentLength,
        &nameLength))
    {
        return NV_FALSE;
    }

    const NvU8* rdn = nameContent;
    const NvU8* nameEnd = nameContent + nameContentLength;
    while (rdn < nameEnd)
    {
        const NvU8* rdnContent;
        size_t rdnContentLength;
        size_t rdnLength;
        if (!getDerElement(
            rdn,
            nameEnd,
            0x31,
            &rdnContent,
            &rdnContentLength,
            &rdnLength) || rdnContentLength == 0)
        {
            return NV_FALSE;
        }

        const NvU8* attribute = rdnContent;
        const NvU8* rdnEnd = rdnContent + rdnContentLength;
        while (attribute < rdnEnd)
        {
            const NvU8* attributeContent;
            size_t attributeContentLength;
            size_t attributeLength;
            if (!getDerElement(
                attribute,
                rdnEnd,
                0x30,
                &attributeContent,
                &attributeContentLength,
                &attributeLength))
            {
                return NV_FALSE;
            }

            const NvU8* oidContent;
            size_t oidContentLength;
            size_t oidLength;
            const NvU8* attributeEnd =
                attributeContent + attributeContentLength;
            if (!getDerElement(
                attributeContent,
                attributeEnd,
                0x06,
                &oidContent,
                &oidContentLength,
                &oidLength) || oidContentLength == 0)
            {
                return NV_FALSE;
            }

            const NvU8* value = attributeContent + oidLength;
            const NvU8* valueContent;
            size_t valueContentLength;
            size_t valueLength;
            if (value == attributeEnd ||
                !getDerElement(
                    value,
                    attributeEnd,
                    *value,
                    &valueContent,
                    &valueContentLength,
                    &valueLength) ||
                value + valueLength != attributeEnd)
            {
                return NV_FALSE;
            }

            attribute += attributeLength;
        }
        rdn += rdnLength;
    }

    return rdn == nameEnd;
}

static NvBool getCertificateName(
    const NvU8* certificate,
    size_t certificateLength,
    NvBool getSubject,
    NvU8* name,
    size_t* nameLength)
{
    if (nameLength == NULL)
    {
        return NV_FALSE;
    }
    const size_t nameCapacity = *nameLength;
    *nameLength = 0;

    const NvU8* field;
    const NvU8* fieldsEnd;
    size_t version;
    if (!getCertificateTbsFields(
            certificate,
            certificateLength,
            &field,
            &fieldsEnd) ||
        !libspdm_x509_get_version(
            certificate,
            certificateLength,
            &version))
    {
        return NV_FALSE;
    }

    const NvU8* ignoredContent;
    size_t ignoredContentLength;
    size_t fieldLength;
    if (*field == 0xa0)
    {
        if (!getDerElement(
            field,
            fieldsEnd,
            0xa0,
            &ignoredContent,
            &ignoredContentLength,
            &fieldLength))
        {
            return NV_FALSE;
        }
        field += fieldLength;
    }

    if (!getDerElement(
        field,
        fieldsEnd,
        0x02,
        &ignoredContent,
        &ignoredContentLength,
        &fieldLength))
    {
        return NV_FALSE;
    }
    field += fieldLength;

    if (!getDerElement(
        field,
        fieldsEnd,
        0x30,
        &ignoredContent,
        &ignoredContentLength,
        &fieldLength))
    {
        return NV_FALSE;
    }
    field += fieldLength;

    const NvU8* nameContent;
    size_t nameContentLength;
    size_t encodedNameLength;
    if (!getDerElement(
        field,
        fieldsEnd,
        0x30,
        &nameContent,
        &nameContentLength,
        &encodedNameLength))
    {
        return NV_FALSE;
    }

    if (getSubject)
    {
        field += encodedNameLength;
        if (!getDerElement(
            field,
            fieldsEnd,
            0x30,
            &ignoredContent,
            &ignoredContentLength,
            &fieldLength))
        {
            return NV_FALSE;
        }
        field += fieldLength;

        if (!getDerElement(
            field,
            fieldsEnd,
            0x30,
            &nameContent,
            &nameContentLength,
            &encodedNameLength))
        {
            return NV_FALSE;
        }
    }

    if (!validateDerName(field, fieldsEnd))
    {
        return NV_FALSE;
    }

    *nameLength = encodedNameLength;
    if (name == NULL || nameCapacity < encodedNameLength)
    {
        return NV_FALSE;
    }

    for (size_t index = 0; index < encodedNameLength; ++index)
    {
        name[index] = field[index];
    }
    return NV_TRUE;
}

NvBool libspdm_x509_get_issuer_name(
    const NvU8* certificate,
    size_t certificateLength,
    NvU8* issuer,
    size_t* issuerLength)
{
    return getCertificateName(
        certificate,
        certificateLength,
        NV_FALSE,
        issuer,
        issuerLength);
}

NvBool libspdm_x509_get_subject_name(
    const NvU8* certificate,
    size_t certificateLength,
    NvU8* subject,
    size_t* subjectLength)
{
    return getCertificateName(
        certificate,
        certificateLength,
        NV_TRUE,
        subject,
        subjectLength);
}

NvBool libspdm_x509_get_cert_from_cert_chain(
    const NvU8* certificateChain,
    size_t certificateChainLength,
    NvS32 certificateIndex,
    const NvU8** certificate,
    size_t* certificateLength)
{
    if (certificateChain == NULL || certificateChainLength == 0 ||
        certificateIndex < -1 || certificate == NULL ||
        certificateLength == NULL)
    {
        return NV_FALSE;
    }

    const NvU8* end = certificateChain + certificateChainLength;
    const NvU8* current = certificateChain;
    const NvU8* last = NULL;
    size_t lastLength = 0;
    NvS32 currentIndex = 0;

    while (current < end)
    {
        NvU8* mutableCurrent = (NvU8*)current;
        size_t currentLength = 0;
        if (!libspdm_asn1_get_tag(
            &mutableCurrent,
            end,
            &currentLength,
            0x30) || currentLength == 0)
        {
            return NV_FALSE;
        }

        if (currentIndex == certificateIndex)
        {
            *certificate = current;
            *certificateLength = currentLength;
            return NV_TRUE;
        }

        last = current;
        lastLength = currentLength;
        current += currentLength;
        ++currentIndex;
    }

    if (current != end || certificateIndex != -1 || last == NULL)
    {
        return NV_FALSE;
    }

    *certificate = last;
    *certificateLength = lastLength;
    return NV_TRUE;
}

static NvBool decodeBase64Character(
    NvU8 character,
    NvU8* value,
    NvBool* padding)
{
    *padding = NV_FALSE;
    if (character >= 'A' && character <= 'Z')
    {
        *value = character - 'A';
    }
    else if (character >= 'a' && character <= 'z')
    {
        *value = character - 'a' + 26;
    }
    else if (character >= '0' && character <= '9')
    {
        *value = character - '0' + 52;
    }
    else if (character == '+' || character == '-')
    {
        *value = 62;
    }
    else if (character == '/' || character == '_')
    {
        *value = 63;
    }
    else if (character == '=')
    {
        *value = 0;
        *padding = NV_TRUE;
    }
    else
    {
        return NV_FALSE;
    }

    return NV_TRUE;
}

NvBool libspdm_decode_base64(
    const NvU8* source,
    NvU8* destination,
    size_t sourceLength,
    size_t* destinationLength)
{
    if (source == NULL || destination == NULL || destinationLength == NULL)
    {
        return NV_FALSE;
    }

    const size_t capacity = *destinationLength;
    size_t produced = 0;
    NvU8 values[4];
    NvBool padding[4];
    NvU32 quartetLength = 0;
    NvBool finalQuartet = NV_FALSE;

    for (size_t index = 0; index < sourceLength; ++index)
    {
        const NvU8 character = source[index];
        if (character == ' ' || character == '\t' ||
            character == '\r' || character == '\n')
        {
            continue;
        }
        if (finalQuartet || !decodeBase64Character(
            character,
            &values[quartetLength],
            &padding[quartetLength]))
        {
            goto failure;
        }

        ++quartetLength;
        if (quartetLength != 4)
        {
            continue;
        }

        if (padding[0] || padding[1] || (padding[2] && !padding[3]))
        {
            goto failure;
        }

        size_t bytes = 3;
        if (padding[2])
        {
            if ((values[1] & 0x0f) != 0)
            {
                goto failure;
            }
            bytes = 1;
            finalQuartet = NV_TRUE;
        }
        else if (padding[3])
        {
            if ((values[2] & 0x03) != 0)
            {
                goto failure;
            }
            bytes = 2;
            finalQuartet = NV_TRUE;
        }

        if (bytes > capacity - produced)
        {
            goto failure;
        }

        destination[produced++] = (values[0] << 2) | (values[1] >> 4);
        if (bytes > 1)
        {
            destination[produced++] = (values[1] << 4) | (values[2] >> 2);
        }
        if (bytes > 2)
        {
            destination[produced++] = (values[2] << 6) | values[3];
        }
        quartetLength = 0;
    }

    if (quartetLength != 0)
    {
        goto failure;
    }

    *destinationLength = produced;
    return NV_TRUE;

failure:
    *destinationLength = 0;
    return NV_FALSE;
}