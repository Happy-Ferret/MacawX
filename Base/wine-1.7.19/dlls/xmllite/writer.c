/*
 * IXmlWriter implementation
 *
 * Copyright 2011 Alistair Leslie-Hughes
 * Copyright 2014 Nikolay Sivov for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#define COBJMACROS

#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "xmllite.h"
#include "xmllite_private.h"
#include "initguid.h"

#include "wine/debug.h"
#include "wine/unicode.h"

WINE_DEFAULT_DEBUG_CHANNEL(xmllite);

/* not defined in public headers */
DEFINE_GUID(IID_IXmlWriterOutput, 0xc1131708, 0x0f59, 0x477f, 0x93, 0x59, 0x7d, 0x33, 0x24, 0x51, 0xbc, 0x1a);

static const WCHAR closepiW[] = {'?','>'};

struct output_buffer
{
    char *data;
    unsigned int allocated;
    unsigned int written;
    UINT codepage;
};

typedef enum
{
    XmlWriterState_Initial,      /* output is not set yet */
    XmlWriterState_Ready,        /* SetOutput() was called, ready to start */
    XmlWriterState_PIDocStarted, /* document was started with manually added 'xml' PI */
    XmlWriterState_DocStarted    /* document was started with WriteStartDocument() */
} XmlWriterState;

typedef struct
{
    IXmlWriterOutput IXmlWriterOutput_iface;
    LONG ref;
    IUnknown *output;
    ISequentialStream *stream;
    IMalloc *imalloc;
    xml_encoding encoding;
    struct output_buffer buffer;
} xmlwriteroutput;

static const struct IUnknownVtbl xmlwriteroutputvtbl;

typedef struct _xmlwriter
{
    IXmlWriter IXmlWriter_iface;
    LONG ref;
    IMalloc *imalloc;
    xmlwriteroutput *output;
    BOOL indent;
    BOOL bom;
    BOOL omitxmldecl;
    XmlConformanceLevel conformance;
    XmlWriterState state;
} xmlwriter;

static inline xmlwriter *impl_from_IXmlWriter(IXmlWriter *iface)
{
    return CONTAINING_RECORD(iface, xmlwriter, IXmlWriter_iface);
}

static inline xmlwriteroutput *impl_from_IXmlWriterOutput(IXmlWriterOutput *iface)
{
    return CONTAINING_RECORD(iface, xmlwriteroutput, IXmlWriterOutput_iface);
}

static const char *debugstr_writer_prop(XmlWriterProperty prop)
{
    static const char * const prop_names[] =
    {
        "MultiLanguage",
        "Indent",
        "ByteOrderMark",
        "OmitXmlDeclaration",
        "ConformanceLevel"
    };

    if (prop > _XmlWriterProperty_Last)
        return wine_dbg_sprintf("unknown property=%d", prop);

    return prop_names[prop];
}

/* writer output memory allocation functions */
static inline void *writeroutput_alloc(xmlwriteroutput *output, size_t len)
{
    return m_alloc(output->imalloc, len);
}

static inline void writeroutput_free(xmlwriteroutput *output, void *mem)
{
    m_free(output->imalloc, mem);
}

static inline void *writeroutput_realloc(xmlwriteroutput *output, void *mem, size_t len)
{
    return m_realloc(output->imalloc, mem, len);
}

/* writer memory allocation functions */
static inline void *writer_alloc(xmlwriter *writer, size_t len)
{
    return m_alloc(writer->imalloc, len);
}

static inline void writer_free(xmlwriter *writer, void *mem)
{
    m_free(writer->imalloc, mem);
}

static HRESULT init_output_buffer(xmlwriteroutput *output)
{
    struct output_buffer *buffer = &output->buffer;
    const int initial_len = 0x2000;
    HRESULT hr;
    UINT cp;

    hr = get_code_page(output->encoding, &cp);
    if (FAILED(hr)) return hr;

    buffer->data = writeroutput_alloc(output, initial_len);
    if (!buffer->data) return E_OUTOFMEMORY;

    memset(buffer->data, 0, 4);
    buffer->allocated = initial_len;
    buffer->written = 0;
    buffer->codepage = cp;

    return S_OK;
}

static void free_output_buffer(xmlwriteroutput *output)
{
    struct output_buffer *buffer = &output->buffer;
    writeroutput_free(output, buffer->data);
    buffer->data = NULL;
    buffer->allocated = 0;
    buffer->written = 0;
}

static HRESULT grow_output_buffer(xmlwriteroutput *output, int length)
{
    struct output_buffer *buffer = &output->buffer;
    /* grow if needed, plus 4 bytes to be sure null terminator will fit in */
    if (buffer->allocated < buffer->written + length + 4) {
        int grown_size = max(2*buffer->allocated, buffer->allocated + length);
        char *ptr = writeroutput_realloc(output, buffer->data, grown_size);
        if (!ptr) return E_OUTOFMEMORY;
        buffer->data = ptr;
        buffer->allocated = grown_size;
    }

    return S_OK;
}

static HRESULT write_output_buffer(xmlwriteroutput *output, const WCHAR *data, int len)
{
    struct output_buffer *buffer = &output->buffer;
    int length;
    HRESULT hr;
    char *ptr;

    if (buffer->codepage != ~0) {
        length = WideCharToMultiByte(buffer->codepage, 0, data, len, NULL, 0, NULL, NULL);
        hr = grow_output_buffer(output, length);
        if (FAILED(hr)) return hr;
        ptr = buffer->data + buffer->written;
        length = WideCharToMultiByte(buffer->codepage, 0, data, len, ptr, length, NULL, NULL);
        buffer->written += len == -1 ? length-1 : length;
    }
    else {
        /* WCHAR data just copied */
        length = len == -1 ? strlenW(data) : len;
        if (length) {
            length *= sizeof(WCHAR);

            hr = grow_output_buffer(output, length);
            if (FAILED(hr)) return hr;
            ptr = buffer->data + buffer->written;

            memcpy(ptr, data, length);
            buffer->written += length;
            ptr += length;
            /* null termination */
            *(WCHAR*)ptr = 0;
        }
    }

    return S_OK;
}

static HRESULT write_output_buffer_quoted(xmlwriteroutput *output, const WCHAR *data, int len)
{
    static const WCHAR quotW[] = {'"'};
    write_output_buffer(output, quotW, 1);
    write_output_buffer(output, data, len);
    write_output_buffer(output, quotW, 1);
    return S_OK;
}

static void writeroutput_release_stream(xmlwriteroutput *writeroutput)
{
    if (writeroutput->stream) {
        ISequentialStream_Release(writeroutput->stream);
        writeroutput->stream = NULL;
    }
}

static inline HRESULT writeroutput_query_for_stream(xmlwriteroutput *writeroutput)
{
    HRESULT hr;

    writeroutput_release_stream(writeroutput);
    hr = IUnknown_QueryInterface(writeroutput->output, &IID_IStream, (void**)&writeroutput->stream);
    if (hr != S_OK)
        hr = IUnknown_QueryInterface(writeroutput->output, &IID_ISequentialStream, (void**)&writeroutput->stream);

    return hr;
}

static HRESULT writeroutput_flush_stream(xmlwriteroutput *output)
{
    struct output_buffer *buffer;
    ULONG written, offset = 0;
    HRESULT hr;

    if (!output || !output->stream)
        return S_OK;

    buffer = &output->buffer;

    /* It will loop forever until everything is written or an error occured. */
    do {
        written = 0;
        hr = ISequentialStream_Write(output->stream, buffer->data + offset, buffer->written, &written);
        if (FAILED(hr)) {
            WARN("write to stream failed (0x%08x)\n", hr);
            buffer->written = 0;
            return hr;
        }

        offset += written;
        buffer->written -= written;
    } while (buffer->written > 0);

    return S_OK;
}

static HRESULT WINAPI xmlwriter_QueryInterface(IXmlWriter *iface, REFIID riid, void **ppvObject)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    TRACE("%p %s %p\n", This, debugstr_guid(riid), ppvObject);

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IXmlWriter))
    {
        *ppvObject = iface;
    }

    IXmlWriter_AddRef(iface);

    return S_OK;
}

static ULONG WINAPI xmlwriter_AddRef(IXmlWriter *iface)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);
    TRACE("%p\n", This);
    return InterlockedIncrement(&This->ref);
}

static ULONG WINAPI xmlwriter_Release(IXmlWriter *iface)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);
    LONG ref;

    TRACE("%p\n", This);

    ref = InterlockedDecrement(&This->ref);
    if (ref == 0) {
        IMalloc *imalloc = This->imalloc;

        IXmlWriter_Flush(iface);
        if (This->output) IUnknown_Release(&This->output->IXmlWriterOutput_iface);
        writer_free(This, This);
        if (imalloc) IMalloc_Release(imalloc);
    }

    return ref;
}

/*** IXmlWriter methods ***/
static HRESULT WINAPI xmlwriter_SetOutput(IXmlWriter *iface, IUnknown *output)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);
    IXmlWriterOutput *writeroutput;
    HRESULT hr;

    TRACE("(%p)->(%p)\n", This, output);

    if (This->output) {
        writeroutput_release_stream(This->output);
        IUnknown_Release(&This->output->IXmlWriterOutput_iface);
        This->output = NULL;
    }

    /* just reset current output */
    if (!output) {
        This->state = XmlWriterState_Initial;
        return S_OK;
    }

    /* now try IXmlWriterOutput, ISequentialStream, IStream */
    hr = IUnknown_QueryInterface(output, &IID_IXmlWriterOutput, (void**)&writeroutput);
    if (hr == S_OK) {
        if (writeroutput->lpVtbl == &xmlwriteroutputvtbl)
            This->output = impl_from_IXmlWriterOutput(writeroutput);
        else {
            ERR("got external IXmlWriterOutput implementation: %p, vtbl=%p\n",
                writeroutput, writeroutput->lpVtbl);
            IUnknown_Release(writeroutput);
            return E_FAIL;

        }
    }

    if (hr != S_OK || !writeroutput) {
        /* create IXmlWriterOutput basing on supplied interface */
        hr = CreateXmlWriterOutputWithEncodingName(output, This->imalloc, NULL, &writeroutput);
        if (hr != S_OK) return hr;
        This->output = impl_from_IXmlWriterOutput(writeroutput);
    }

    This->state = XmlWriterState_Ready;
    return writeroutput_query_for_stream(This->output);
}

static HRESULT WINAPI xmlwriter_GetProperty(IXmlWriter *iface, UINT property, LONG_PTR *value)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_writer_prop(property), value);

    if (!value) return E_INVALIDARG;

    switch (property)
    {
        case XmlWriterProperty_Indent:
            *value = This->indent;
            break;
        case XmlWriterProperty_ByteOrderMark:
            *value = This->bom;
            break;
        case XmlWriterProperty_OmitXmlDeclaration:
            *value = This->omitxmldecl;
            break;
        case XmlWriterProperty_ConformanceLevel:
            *value = This->conformance;
            break;
        default:
            FIXME("Unimplemented property (%u)\n", property);
            return E_NOTIMPL;
    }

    return S_OK;
}

static HRESULT WINAPI xmlwriter_SetProperty(IXmlWriter *iface, UINT nProperty, LONG_PTR pValue)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %u %lu\n", This, nProperty, pValue);

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteAttributes(IXmlWriter *iface, IXmlReader *pReader,
                                  BOOL fWriteDefaultAttributes)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %p %d\n", This, pReader, fWriteDefaultAttributes);

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteAttributeString(IXmlWriter *iface, LPCWSTR pwszPrefix,
                                       LPCWSTR pwszLocalName, LPCWSTR pwszNamespaceUri,
                                       LPCWSTR pwszValue)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %s %s %s %s\n", This, wine_dbgstr_w(pwszPrefix), wine_dbgstr_w(pwszLocalName),
                        wine_dbgstr_w(pwszNamespaceUri), wine_dbgstr_w(pwszValue));

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteCData(IXmlWriter *iface, LPCWSTR pwszText)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %s\n", This, wine_dbgstr_w(pwszText));

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteCharEntity(IXmlWriter *iface, WCHAR wch)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteChars(IXmlWriter *iface, const WCHAR *pwch, UINT cwch)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %s %d\n", This, wine_dbgstr_w(pwch), cwch);

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteComment(IXmlWriter *iface, LPCWSTR pwszComment)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteDocType(IXmlWriter *iface, LPCWSTR pwszName, LPCWSTR pwszPublicId,
                               LPCWSTR pwszSystemId, LPCWSTR pwszSubset)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %s %s %s %s\n", This, wine_dbgstr_w(pwszName), wine_dbgstr_w(pwszPublicId),
                        wine_dbgstr_w(pwszSystemId), wine_dbgstr_w(pwszSubset));

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteElementString(IXmlWriter *iface, LPCWSTR pwszPrefix,
                                     LPCWSTR pwszLocalName, LPCWSTR pwszNamespaceUri,
                                     LPCWSTR pwszValue)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %s %s %s %s\n", This, wine_dbgstr_w(pwszPrefix), wine_dbgstr_w(pwszLocalName),
                        wine_dbgstr_w(pwszNamespaceUri), wine_dbgstr_w(pwszValue));

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteEndDocument(IXmlWriter *iface)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p\n", This);

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteEndElement(IXmlWriter *iface)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p\n", This);

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteEntityRef(IXmlWriter *iface, LPCWSTR pwszName)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %s\n", This, wine_dbgstr_w(pwszName));

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteFullEndElement(IXmlWriter *iface)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p\n", This);

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteName(IXmlWriter *iface, LPCWSTR pwszName)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %s\n", This, wine_dbgstr_w(pwszName));

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteNmToken(IXmlWriter *iface, LPCWSTR pwszNmToken)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %s\n", This, wine_dbgstr_w(pwszNmToken));

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteNode(IXmlWriter *iface, IXmlReader *pReader,
                            BOOL fWriteDefaultAttributes)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %p %d\n", This, pReader, fWriteDefaultAttributes);

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteNodeShallow(IXmlWriter *iface, IXmlReader *pReader,
                                   BOOL fWriteDefaultAttributes)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %p %d\n", This, pReader, fWriteDefaultAttributes);

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteProcessingInstruction(IXmlWriter *iface, LPCWSTR name,
                                             LPCWSTR text)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);
    static const WCHAR xmlW[] = {'x','m','l',0};
    static const WCHAR openpiW[] = {'<','?'};
    static const WCHAR spaceW[] = {' '};

    TRACE("(%p)->(%s %s)\n", This, wine_dbgstr_w(name), wine_dbgstr_w(text));

    if (This->state == XmlWriterState_Initial)
        return E_UNEXPECTED;

    if (This->state == XmlWriterState_DocStarted && !strcmpW(name, xmlW))
        return WR_E_INVALIDACTION;

    write_output_buffer(This->output, openpiW, sizeof(openpiW)/sizeof(WCHAR));
    write_output_buffer(This->output, name, -1);
    write_output_buffer(This->output, spaceW, 1);
    write_output_buffer(This->output, text, -1);
    write_output_buffer(This->output, closepiW, sizeof(closepiW)/sizeof(WCHAR));

    if (!strcmpW(name, xmlW))
        This->state = XmlWriterState_PIDocStarted;

    return S_OK;
}

static HRESULT WINAPI xmlwriter_WriteQualifiedName(IXmlWriter *iface, LPCWSTR pwszLocalName,
                                     LPCWSTR pwszNamespaceUri)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %s %s\n", This, wine_dbgstr_w(pwszLocalName), wine_dbgstr_w(pwszNamespaceUri));

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteRaw(IXmlWriter *iface, LPCWSTR pwszData)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %s\n", This, wine_dbgstr_w(pwszData));

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteRawChars(IXmlWriter *iface,  const WCHAR *pwch, UINT cwch)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %s %d\n", This, wine_dbgstr_w(pwch), cwch);

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteStartDocument(IXmlWriter *iface, XmlStandalone standalone)
{
    static const WCHAR versionW[] = {'<','?','x','m','l',' ','v','e','r','s','i','o','n','=','"','1','.','0','"'};
    static const WCHAR encodingW[] = {' ','e','n','c','o','d','i','n','g','='};
    xmlwriter *This = impl_from_IXmlWriter(iface);

    TRACE("(%p)->(%d)\n", This, standalone);

    switch (This->state)
    {
    case XmlWriterState_Initial:
        return E_UNEXPECTED;
    case XmlWriterState_PIDocStarted:
        This->state = XmlWriterState_DocStarted;
        return S_OK;
    case XmlWriterState_DocStarted:
        return WR_E_INVALIDACTION;
    default:
        ;
    }

    /* version */
    write_output_buffer(This->output, versionW, sizeof(versionW)/sizeof(WCHAR));

    /* encoding */
    write_output_buffer(This->output, encodingW, sizeof(encodingW)/sizeof(WCHAR));
    write_output_buffer_quoted(This->output, get_encoding_name(This->output->encoding), -1);

    /* standalone */
    if (standalone == XmlStandalone_Omit)
        write_output_buffer(This->output, closepiW, sizeof(closepiW)/sizeof(WCHAR));
    else {
        static const WCHAR standaloneW[] = {' ','s','t','a','n','d','a','l','o','n','e','=','\"'};
        static const WCHAR yesW[] = {'y','e','s','\"','?','>'};
        static const WCHAR noW[] = {'n','o','\"','?','>'};

        write_output_buffer(This->output, standaloneW, sizeof(standaloneW)/sizeof(WCHAR));
        if (standalone == XmlStandalone_Yes)
            write_output_buffer(This->output, yesW, sizeof(yesW)/sizeof(WCHAR));
        else
            write_output_buffer(This->output, noW, sizeof(noW)/sizeof(WCHAR));
    }

    This->state = XmlWriterState_DocStarted;
    return S_OK;
}

static HRESULT WINAPI xmlwriter_WriteStartElement(IXmlWriter *iface, LPCWSTR pwszPrefix,
                                    LPCWSTR pwszLocalName, LPCWSTR pwszNamespaceUri)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %s %s %s\n", This, wine_dbgstr_w(pwszPrefix), wine_dbgstr_w(pwszLocalName),
                wine_dbgstr_w(pwszNamespaceUri));

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteString(IXmlWriter *iface, LPCWSTR pwszText)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %s\n", This, wine_dbgstr_w(pwszText));

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteSurrogateCharEntity(IXmlWriter *iface, WCHAR wchLow, WCHAR wchHigh)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %d %d\n", This, wchLow, wchHigh);

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_WriteWhitespace(IXmlWriter *iface, LPCWSTR pwszWhitespace)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    FIXME("%p %s\n", This, wine_dbgstr_w(pwszWhitespace));

    return E_NOTIMPL;
}

static HRESULT WINAPI xmlwriter_Flush(IXmlWriter *iface)
{
    xmlwriter *This = impl_from_IXmlWriter(iface);

    TRACE("%p\n", This);

    return writeroutput_flush_stream(This->output);
}

static const struct IXmlWriterVtbl xmlwriter_vtbl =
{
    xmlwriter_QueryInterface,
    xmlwriter_AddRef,
    xmlwriter_Release,
    xmlwriter_SetOutput,
    xmlwriter_GetProperty,
    xmlwriter_SetProperty,
    xmlwriter_WriteAttributes,
    xmlwriter_WriteAttributeString,
    xmlwriter_WriteCData,
    xmlwriter_WriteCharEntity,
    xmlwriter_WriteChars,
    xmlwriter_WriteComment,
    xmlwriter_WriteDocType,
    xmlwriter_WriteElementString,
    xmlwriter_WriteEndDocument,
    xmlwriter_WriteEndElement,
    xmlwriter_WriteEntityRef,
    xmlwriter_WriteFullEndElement,
    xmlwriter_WriteName,
    xmlwriter_WriteNmToken,
    xmlwriter_WriteNode,
    xmlwriter_WriteNodeShallow,
    xmlwriter_WriteProcessingInstruction,
    xmlwriter_WriteQualifiedName,
    xmlwriter_WriteRaw,
    xmlwriter_WriteRawChars,
    xmlwriter_WriteStartDocument,
    xmlwriter_WriteStartElement,
    xmlwriter_WriteString,
    xmlwriter_WriteSurrogateCharEntity,
    xmlwriter_WriteWhitespace,
    xmlwriter_Flush
};

/** IXmlWriterOutput **/
static HRESULT WINAPI xmlwriteroutput_QueryInterface(IXmlWriterOutput *iface, REFIID riid, void** ppvObject)
{
    xmlwriteroutput *This = impl_from_IXmlWriterOutput(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), ppvObject);

    if (IsEqualGUID(riid, &IID_IXmlWriterOutput) ||
        IsEqualGUID(riid, &IID_IUnknown))
    {
        *ppvObject = iface;
    }
    else
    {
        WARN("interface %s not implemented\n", debugstr_guid(riid));
        *ppvObject = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef(iface);

    return S_OK;
}

static ULONG WINAPI xmlwriteroutput_AddRef(IXmlWriterOutput *iface)
{
    xmlwriteroutput *This = impl_from_IXmlWriterOutput(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p)->(%d)\n", This, ref);
    return ref;
}

static ULONG WINAPI xmlwriteroutput_Release(IXmlWriterOutput *iface)
{
    xmlwriteroutput *This = impl_from_IXmlWriterOutput(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p)->(%d)\n", This, ref);

    if (ref == 0)
    {
        IMalloc *imalloc = This->imalloc;
        if (This->output) IUnknown_Release(This->output);
        if (This->stream) ISequentialStream_Release(This->stream);
        free_output_buffer(This);
        writeroutput_free(This, This);
        if (imalloc) IMalloc_Release(imalloc);
    }

    return ref;
}

static const struct IUnknownVtbl xmlwriteroutputvtbl =
{
    xmlwriteroutput_QueryInterface,
    xmlwriteroutput_AddRef,
    xmlwriteroutput_Release
};

HRESULT WINAPI CreateXmlWriter(REFIID riid, void **obj, IMalloc *imalloc)
{
    xmlwriter *writer;

    TRACE("(%s, %p, %p)\n", wine_dbgstr_guid(riid), obj, imalloc);

    if (!IsEqualGUID(riid, &IID_IXmlWriter))
    {
        ERR("Unexpected IID requested -> (%s)\n", wine_dbgstr_guid(riid));
        return E_FAIL;
    }

    if (imalloc)
        writer = IMalloc_Alloc(imalloc, sizeof(*writer));
    else
        writer = heap_alloc(sizeof(*writer));
    if(!writer) return E_OUTOFMEMORY;

    writer->IXmlWriter_iface.lpVtbl = &xmlwriter_vtbl;
    writer->ref = 1;
    writer->imalloc = imalloc;
    if (imalloc) IMalloc_AddRef(imalloc);
    writer->output = NULL;
    writer->indent = FALSE;
    writer->bom = TRUE;
    writer->omitxmldecl = FALSE;
    writer->conformance = XmlConformanceLevel_Document;
    writer->state = XmlWriterState_Initial;

    *obj = &writer->IXmlWriter_iface;

    TRACE("returning iface %p\n", *obj);

    return S_OK;
}

HRESULT WINAPI CreateXmlWriterOutputWithEncodingName(IUnknown *stream,
                                                     IMalloc *imalloc,
                                                     LPCWSTR encoding,
                                                     IXmlWriterOutput **output)
{
    static const WCHAR utf8W[] = {'U','T','F','-','8',0};
    xmlwriteroutput *writeroutput;
    HRESULT hr;

    TRACE("%p %p %s %p\n", stream, imalloc, debugstr_w(encoding), output);

    if (!stream || !output) return E_INVALIDARG;

    *output = NULL;

    if (imalloc)
        writeroutput = IMalloc_Alloc(imalloc, sizeof(*writeroutput));
    else
        writeroutput = heap_alloc(sizeof(*writeroutput));
    if(!writeroutput) return E_OUTOFMEMORY;

    writeroutput->IXmlWriterOutput_iface.lpVtbl = &xmlwriteroutputvtbl;
    writeroutput->ref = 1;
    writeroutput->imalloc = imalloc;
    if (imalloc) IMalloc_AddRef(imalloc);
    writeroutput->encoding = parse_encoding_name(encoding ? encoding : utf8W, -1);
    writeroutput->stream = NULL;
    hr = init_output_buffer(writeroutput);
    if (FAILED(hr)) {
        IUnknown_Release(&writeroutput->IXmlWriterOutput_iface);
        return hr;
    }

    IUnknown_QueryInterface(stream, &IID_IUnknown, (void**)&writeroutput->output);

    *output = &writeroutput->IXmlWriterOutput_iface;

    TRACE("returning iface %p\n", *output);

    return S_OK;
}
