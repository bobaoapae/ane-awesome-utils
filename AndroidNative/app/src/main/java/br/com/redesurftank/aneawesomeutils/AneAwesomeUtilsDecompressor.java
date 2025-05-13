package br.com.redesurftank.aneawesomeutils;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.zip.GZIPInputStream;
import java.util.zip.InflaterInputStream;

public class AneAwesomeUtilsDecompressor {

    /**
     * Descomprime um array de bytes compactado em ZLIB (padrão do ZLibStream do .NET).
     * Se detectar cabeçalho GZIP (0x1F 0x8B), usa GZIPInputStream.
     */
    public static byte[] decompress(byte[] data) throws IOException {
        try (ByteArrayInputStream bais = new ByteArrayInputStream(data);
             InputStream is = detectStream(bais, data);
             ByteArrayOutputStream baos = new ByteArrayOutputStream()) {

            byte[] buffer = new byte[1024];
            int len;
            while ((len = is.read(buffer)) != -1) {
                baos.write(buffer, 0, len);
            }
            return baos.toByteArray();
        }
    }

    // escolhe InflaterInputStream (ZLIB) ou GZIPInputStream
    private static InputStream detectStream(ByteArrayInputStream bais, byte[] data) throws IOException {
        if (data.length >= 2 && (data[0] & 0xFF) == 0x1F && (data[1] & 0xFF) == 0x8B) {
            return new GZIPInputStream(bais);
        } else {
            return new InflaterInputStream(bais);
        }
    }
}

