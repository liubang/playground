package cn.iliubang.exercises.mtomcat.response;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2017/12/30
 */
public enum ResponseStatus {

    Continue(100, "Continue"),
    SwitchingProtocols(101, "Switching Protocols"),

    OK(200, "OK"),
    Created(201, "Created"),
    Accepted(202, "Accepted"),
    NonAuthoritativeInformation(203, "Non-Authoritative Information"),
    NoContent(204, "No Content"),
    ResetContent(205, "Reset Content"),
    PartialContent(206, "Partial Content"),

    MultipleChoices(300, "Multiple Choices"),
    MovedPermanently(301, "Moved Permanently"),
    Found(302, "Found"),
    SeeOther(303, "See Other"),
    NotModified(304, "Not Modified"),
    UseProxy(305, "Use Proxy"),
    UnUsed(306, "Unused"),
    TemporaryRedirect(307, "Temporary Redirect"),

    BadRequest(400, "Bad Request"),
    Unauthorized(401, "Unauthorized"),
    PaymentRequired(402, "Payment Required"),
    Forbidden(403, "Forbidden"),
    NotFound(404, "Not Found"),
    MethodNotAllowed(405, "Method Not Allowed"),
    NotAcceptable(406, "Not Acceptable"),
    ProxyAuthenticationRequired(407, "Proxy Authentication Required"),
    RequestTimeout(408, "Request Timeout"),
    Conflict(409, "Conflict"),
    Gone(410, "Gone"),
    LengthRequired(411, "Length Required"),
    PreconditionFailed(412, "Precondition Failed"),
    RequestEntityTooLarge(413, "Request Entity Too Large"),
    RequestedURITooLong(414, "Request URI Too Long"),
    UnsupportedMediaType(415, "Unsupported Media Type"),
    RequestRangeNotSatisfiable(416, "Request Range Not Satisfiable"),
    ExpectationFailed(417, "Expectation Failed"),
    ImATeapot(418, "I'm a teapot"),

    InternalServerError(500, "Internal HttpServer Error"),
    NotImplemented(501, "Not Implemented"),
    BadGateway(502, "Bad Gateway"),
    ServiceUnavailable(503, "Service Unavailable"),
    GatewayTimeout(504, "Gateway Timeout"),
    HttpVersionNotSupported(505, "HTTP Version Not Supported"),

    Unknown(400, "Unknown HTTP Status Code");

    private int code;

    private String desc;

    private ResponseStatus(int code, String desc) {
        this.code = code;
        this.desc = desc;
    }

    public int getCode() {
        return code;
    }

    public void setCode(int code) {
        this.code = code;
    }

    public String getDesc() {
        return desc;
    }

    public void setDesc(String desc) {
        this.desc = desc;
    }
}
