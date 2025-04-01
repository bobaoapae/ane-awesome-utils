using System;
using System.Collections.Generic;

namespace AwesomeAneUtils;

public class WebSocketConnectException : AggregateException
{
    public int ResponseCode { get; }
    public Dictionary<string, string> Headers { get; }

    public WebSocketConnectException(string message, IEnumerable<Exception> innerExceptions, int responseCode, Dictionary<string, string> headers)
        : base(message, innerExceptions)
    {
        ResponseCode = responseCode;
        Headers = headers ?? new Dictionary<string, string>();
    }
}
