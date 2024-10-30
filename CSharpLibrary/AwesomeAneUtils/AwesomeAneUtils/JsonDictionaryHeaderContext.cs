using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AwesomeAneUtils;

[JsonSerializable(typeof(Dictionary<string, string>))]
public partial class JsonDictionaryHeaderContext : JsonSerializerContext
{
    
}