this file lists the apis of the http parser, which is totally self-written

*Data Types*
**HTTPRequest**
contains the *method*, *target*, *version*, *body* in string format
and store the headers in the **unordered map** format, both the key and the value is in string format

**HTTPParseError**
which is an enumeration: InvalidStartLine, InvalidHeaderSyntax, InvalidContentLength, UnexpectedEOF, IllegalWhitespace
*(in the left-to-right order)*


*Member Functions:*  
  
**Initializer()**  
accepts the data of the type "std::vector<std::uint8_t>"

**Parse()**
returns the result or the error in the type "std::expected"  
the result is in the HTTPRequest form, while the error is in the HTTPParseError form