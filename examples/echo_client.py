import socket
import sys

try:
    with socket.create_connection(('127.0.0.1', 5555), timeout=5) as connection:
        print('Подключён к серверу. /quit — выход.')
        while True:
            message = input('Сообщение: ')
            if message == '/quit':
                break

            data = (message + '\n').encode('utf-8')
            if len(data) > 64 * 1024:
                print('Сообщение слишком длинное: максимум 64 КиБ.')
                continue
            connection.sendall(data)

            reply = b''
            while len(reply) < len(data):
                part = connection.recv(len(data) - len(reply))
                if not part:
                    raise ConnectionError('Сервер закрыл соединение')
                reply += part
            print('Ответ:', reply.decode('utf-8').rstrip('\n'))
except (EOFError, KeyboardInterrupt):
    pass
except OSError as error:
    print(error, file=sys.stderr)
    sys.exit(1)
