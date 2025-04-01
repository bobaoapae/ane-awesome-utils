package {
import flash.utils.ByteArray;

public class Base64
{

    public function Base64()
    {
        super();
        throw new Error("Base64 class is static container only");
    }

    public static function encode(param1:String):String
    {
        var _loc2_:ByteArray = null;
        _loc2_ = new ByteArray();
        _loc2_.writeUTFBytes(param1);
        return encodeByteArray(_loc2_);
    }

    public static function encodeByteArray(param1:ByteArray):String
    {
        return param1.toBase64();
    }

    public static function decode(param1:String):String
    {
        var _loc2_:ByteArray = null;
        _loc2_ = decodeToByteArray(param1);
        return _loc2_.readUTFBytes(_loc2_.length);
    }

    public static function decodeToByteArray(param1:String):ByteArray
    {
        var data:ByteArray = ByteArray.createFromBase64(param1);
        data.position = 0;
        return data;
    }
}
}
