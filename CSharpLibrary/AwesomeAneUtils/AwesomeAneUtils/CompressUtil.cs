using System;
using System.IO;
using System.IO.Compression;
using ComponentAce.Compression.Libs.zlib;
using CompressionMode = System.IO.Compression.CompressionMode;

namespace AwesomeAneUtils
{
    /// <summary>
    ///     Provides basic functionality to convert data types
    /// </summary>
    public class CompressUtil
    {
        /// <summary>
        ///     Zip compress data
        /// </summary>
        /// <param name="src"></param>
        /// <returns></returns>
        public static byte[] Compress(byte[] src)
        {
            return Compress(src, 0, src.Length);
        }

        /// <summary>
        ///     Zip compress data with offset and length.
        /// </summary>
        /// <param name="src"></param>
        /// <returns></returns>
        public static byte[] Compress(byte[] src, int offset, int length)
        {
            var ms = new MemoryStream();
            Stream s = new ZOutputStream(ms, 9);
            s.Write(src, offset, length);
            s.Close();
            return ms.ToArray();
        }

        public static MemoryStream Compress(ArraySegment<byte> src)
        {
            var ms = new MemoryStream();
            using var sourceStream = new MemoryStream(src.Array!, src.Offset, src.Count);
            using var compressionStream = new ZLibStream(ms, CompressionMode.Compress, true);
            sourceStream.CopyTo(compressionStream);
            return ms;
        }

        public static MemoryStream Uncompress(ArraySegment<byte> src)
        {
            var ms = new MemoryStream();
            using var sourceStream = new MemoryStream(src.Array!, src.Offset, src.Count);
            using var decompressionStream = new ZLibStream(sourceStream, CompressionMode.Decompress, true);
            decompressionStream.CopyTo(ms);
            return ms;
        }
        
        /// <summary>
        ///   Descomprime lendo de qualquer Stream (pode ser UnmanagedMemoryStream).
        /// </summary>
        public static MemoryStream Uncompress(Stream source)
        {
            if (!source.CanSeek)
            {
                var tmp = new MemoryStream();
                source.CopyTo(tmp);
                tmp.Position = 0;
                source = tmp;
            }

            int b0 = source.ReadByte();
            int b1 = source.ReadByte();
            source.Seek(-2, SeekOrigin.Current);

            var output = new MemoryStream();
            if (b0 == 0x1F && b1 == 0x8B)
            {
                using var dec = new GZipStream(source, CompressionMode.Decompress, true);
                dec.CopyTo(output);
            }
            else
            {
                using var dec = new ZLibStream(source, CompressionMode.Decompress, true);
                dec.CopyTo(output);
            }
            output.Position = 0;
            return output;
        }

        /// <summary>
        ///     Zip uncompress data.
        /// </summary>
        /// <param name="src"></param>
        /// <returns></returns>
        public static byte[] Uncompress(byte[] src)
        {
            return Uncompress(src, 0);
        }

        public static byte[] Uncompress(byte[] src, int offset)
        {
            var md = new MemoryStream();
            Stream d = new ZOutputStream(md);
            d.Write(src, offset, src.Length);
            d.Close();
            return md.ToArray();
        }
    }
}