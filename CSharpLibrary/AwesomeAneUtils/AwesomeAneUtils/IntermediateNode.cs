using System.Collections.Generic;

namespace AwesomeAneUtils;


abstract class IntermediateNode
{
}

class ScalarNode : IntermediateNode
{
    public string RawValue { get; set; }
}

class ObjectNode : IntermediateNode
{
    public Dictionary<string, string> Attributes { get; set; } = new Dictionary<string, string>();
    public Dictionary<string, IntermediateNode> Properties { get; set; } = new Dictionary<string, IntermediateNode>();
}

class ArrayNode : IntermediateNode
{
    public List<IntermediateNode> Items { get; set; } = new List<IntermediateNode>();
}