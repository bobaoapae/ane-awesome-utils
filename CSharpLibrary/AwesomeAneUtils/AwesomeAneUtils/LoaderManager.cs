﻿using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Threading.Tasks;
using System.Web;

namespace AwesomeAneUtils;

public class LoaderManager
{
    private static byte[] EmptyResult = [0, 1, 2, 3];

    private static LoaderManager _instance;

    public static LoaderManager Instance => _instance ??= new LoaderManager();

    public bool Initialized { get; private set; }

    private Action<string> _success;
    private Action<string, string> _error;
    private Action<string, string> _progress;
    private Action<string> _writeLog;
    private HttpClient _client;
    private ConcurrentDictionary<Guid, byte[]> _results;

    public void Initialize(Action<string> success, Action<string, string> error, Action<string, string> progress, Action<string> writeLog)
    {
        Initialized = true;
        _success = success;
        _error = error;
        _progress = progress;
        _writeLog = writeLog;
        _client = HappyEyeballsHttp.CreateHttpClient(true, _writeLog);
        _results = new ConcurrentDictionary<Guid, byte[]>();
    }

    public string StartLoad(string url, string method, Dictionary<string, string> variables, Dictionary<string, string> headers)
    {
        var randomId = Guid.NewGuid();

        _ = Task.Run(async () =>
        {
            try
            {
                var request = new HttpRequestMessage(new HttpMethod(method.ToUpper()), url);
                request.Version = HttpVersion.Version20;
                request.VersionPolicy = HttpVersionPolicy.RequestVersionOrLower;
                foreach (var (key, value) in headers)
                {
                    request.Headers.Add(key, value);
                }

                // If method is GET, add variables to the url
                if (request.Method == HttpMethod.Get)
                {
                    var uriBuilder = new UriBuilder(url);
                    var query = HttpUtility.ParseQueryString(uriBuilder.Query);
                    foreach (var (key, value) in variables)
                    {
                        query[key] = value;
                    }

                    uriBuilder.Query = query.ToString()!;
                    request.RequestUri = uriBuilder.Uri;
                }
                else // If method is POST, add variables to the content
                {
                    request.Content = new FormUrlEncodedContent(variables);
                }

                var response = await _client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead);
                if (response.StatusCode >= HttpStatusCode.BadRequest)
                {
                    _ = Task.Run(() =>
                    {
                        try
                        {
                            _error(randomId.ToString(), $"Invalid status code: {response.StatusCode}");
                        }
                        catch (Exception e)
                        {
                            LogAll(e);
                        }
                    });

                    return;
                }

                var contentLength = response.Content.Headers.ContentLength ?? -1;
                var totalBytesRead = 0L;
                var buffer = new byte[8192];

                await using var stream = await response.Content.ReadAsStreamAsync();
                using var memoryStream = new MemoryStream();

                int bytesRead;
                while ((bytesRead = await stream.ReadAsync(buffer)) > 0)
                {
                    memoryStream.Write(buffer, 0, bytesRead);
                    totalBytesRead += bytesRead;

                    // Report progress if content length is known
                    if (contentLength > 0)
                    {
                        _progress(randomId.ToString(), $"{totalBytesRead};{contentLength}");
                    }
                }

                var result = memoryStream.ToArray();
                if (result.Length == 0)
                {
                    result = EmptyResult;
                }

                if (!_results.TryAdd(randomId, result))
                {
                    try
                    {
                        _error(randomId.ToString(), $"Failed to add result for {randomId}");
                    }
                    catch (Exception)
                    {
                        // ignored
                    }

                    return;
                }

                _ = Task.Run(() =>
                {
                    try
                    {
                        _success(randomId.ToString());
                    }
                    catch (Exception e)
                    {
                        LogAll(e);

                        try
                        {
                            _error(randomId.ToString(), e.Message);
                        }
                        catch (Exception)
                        {
                            // ignored
                        }
                    }
                });
            }
            catch (Exception e)
            {
                _ = Task.Run(() =>
                {
                    LogAll(e);

                    try
                    {
                        _error(randomId.ToString(), e.Message);
                    }
                    catch (Exception)
                    {
                        // ignored
                    }
                });
            }
        });

        return randomId.ToString();
    }

    public bool TryGetResult(Guid guid, out byte[] result)
    {
        return _results.TryRemove(guid, out result);
    }

    private void LogAll(Exception exception)
    {
        if (exception == null)
            return;

        try
        {
            var logBuilder = new StringBuilder();

            // Log the main exception
            logBuilder.AppendLine($"Exception: {exception.Message}");
            logBuilder.AppendLine($"Stack Trace: {exception.StackTrace}");

            var inner = exception.InnerException;
            while (inner != null)
            {
                logBuilder.AppendLine($"Inner Exception: {inner.Message}");
                logBuilder.AppendLine($"Inner Stack Trace: {inner.StackTrace}");
                inner = inner.InnerException;
            }

            // Call _writeLog once with the complete log string
            _writeLog(logBuilder.ToString());
        }
        catch (Exception)
        {
            // ignored
        }
    }
}