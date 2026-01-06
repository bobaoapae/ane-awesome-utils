using System;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text; // for X509CertificateLoader

namespace AwesomeAneUtils;

public static class ClientCertificateProvider
{
    private static readonly object Gate = new();
    private static X509Certificate2 _defaultCertificate;
    private static readonly Dictionary<string, X509Certificate2> HostCertificates = new(StringComparer.OrdinalIgnoreCase);

    public static bool HasHostCertificate(string host)
    {
        if (string.IsNullOrWhiteSpace(host))
            return false;

        lock (Gate)
        {
            return HostCertificates.ContainsKey(host);
        }
    }

    public static X509Certificate2 GetCertificate(string host = null)
    {
        lock (Gate)
        {
            if (!string.IsNullOrWhiteSpace(host) && HostCertificates.TryGetValue(host, out var byHost))
            {
                return byHost;
            }

            return _defaultCertificate;
        }
    }

    public static bool TryConfigureDefault(string base64Pfx, string password, out string error)
    {
        return TryConfigureInternal(null, base64Pfx, password, out error);
    }

    public static bool TryConfigureHost(string host, string base64Pfx, string password, out string error)
    {
        if (string.IsNullOrWhiteSpace(host))
        {
            error = "Host must be provided for host-specific certificate.";
            return false;
        }

        return TryConfigureInternal(host, base64Pfx, password, out error);
    }

    public static void ClearAll()
    {
        lock (Gate)
        {
            _defaultCertificate?.Dispose();
            _defaultCertificate = null;
            foreach (var kvp in HostCertificates)
            {
                kvp.Value?.Dispose();
            }
            HostCertificates.Clear();
        }
    }

    private static bool TryConfigureInternal(string host, string base64Pfx, string password, out string error)
    {
        error = null;
        try
        {
            if (string.IsNullOrWhiteSpace(base64Pfx))
            {
                lock (Gate)
                {
                    if (string.IsNullOrWhiteSpace(host))
                    {
                        _defaultCertificate?.Dispose();
                        _defaultCertificate = null;
                    }
                    else
                    {
                        if (HostCertificates.TryGetValue(host, out var existing))
                        {
                            existing?.Dispose();
                            HostCertificates.Remove(host);
                        }
                    }
                }

                return true;
            }

            var cert = LoadFromPemOrBase64Pem(base64Pfx, password);

            lock (Gate)
            {
                if (string.IsNullOrWhiteSpace(host))
                {
                    _defaultCertificate?.Dispose();
                    _defaultCertificate = cert;
                }
                else
                {
                    if (HostCertificates.TryGetValue(host, out var existing))
                    {
                        existing?.Dispose();
                    }

                    HostCertificates[host] = cert;
                }
            }

            return true;
        }
        catch (Exception ex)
        {
            error = ex.Message;
            return false;
        }
    }

    private static X509Certificate2 LoadFromPemOrBase64Pem(string pemOrBase64, string password)
    {
        // Accept PEM text directly or Base64-encoded PEM content.
        var pemText = pemOrBase64.Contains("-----BEGIN", StringComparison.Ordinal)
            ? pemOrBase64
            : Encoding.UTF8.GetString(Convert.FromBase64String(pemOrBase64));

        // Extract first certificate block and first private key block.
        var certBlock = ExtractPemBlock(pemText, "CERTIFICATE");
        var keyBlock = ExtractKeyBlock(pemText);

        if (string.IsNullOrWhiteSpace(certBlock) || string.IsNullOrWhiteSpace(keyBlock))
            throw new InvalidOperationException("PEM must contain both certificate and private key blocks.");

        return string.IsNullOrEmpty(password)
            ? X509Certificate2.CreateFromPem(certBlock, keyBlock)
            : X509Certificate2.CreateFromEncryptedPem(certBlock, keyBlock, password);
    }

    private static string ExtractKeyBlock(string pemText)
    {
        // Try common key headers in order.
        var headers = new[] { "ENCRYPTED PRIVATE KEY", "PRIVATE KEY", "RSA PRIVATE KEY", "EC PRIVATE KEY" };
        foreach (var header in headers)
        {
            var block = ExtractPemBlock(pemText, header);
            if (!string.IsNullOrWhiteSpace(block))
                return block;
        }

        return null;
    }

    private static string ExtractPemBlock(string pemText, string label)
    {
        var begin = $"-----BEGIN {label}-----";
        var end = $"-----END {label}-----";

        var startIdx = pemText.IndexOf(begin, StringComparison.Ordinal);
        if (startIdx < 0) return null;
        startIdx += begin.Length;

        var endIdx = pemText.IndexOf(end, startIdx, StringComparison.Ordinal);
        if (endIdx < 0) return null;

        var content = pemText.Substring(startIdx, endIdx - startIdx);
        return begin + content + end;
    }
}
