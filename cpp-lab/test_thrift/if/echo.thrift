namespace cpp2 echo

enum Status {
    OK = 1,
    ERR = 0,
}

struct EchoRequest {
    1: string message
}

struct EchoResponse {
    1: string message
}

exception Exception {
    1: string reason
    2: Status status
}

service EchoService {
    EchoResponse echo(1: EchoRequest request) throws (1: Exception e)
}
