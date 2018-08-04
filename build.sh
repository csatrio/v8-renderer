reset
g++ -std=c++14 -I. -I/usr/include -I/usr/local/include -L/usr/lib -L/usr/local/lib test-uv.cc -o test_uv  -Wl,--start-group /home/csatrio/nodeuv/nodeuv-http/build/out/Release/obj.target/deps/libuv/libuv.a /home/csatrio/nodeuv/nodeuv-http/build/out/Release/obj.target/deps/http-parser/libhttp_parser.a /home/csatrio/nodeuv/nodeuv-http/build/out/Release/obj.target/libnodeuv-http.a -Wl,--end-group -latomic -ldl -lpthread -lrt -lm -lcurl
chmod 777 test_uv;
./test_uv;
echo;
