## Iot Weather Station
IoT Weather statuib used to collect Environmental data. This data is accessable by a webserver ran on a MKR 1010. The MKR has a compressed web page that if a user connects to the server using a web browser will allow users to view the data collected by the station.

- AP Mode
If the station does not have the local area networks credentials saved it will enter access point mode where a user can connect to the station using wifi and enter the credentials into a webpage. Once the station has successfully connected the lan it will automatically
put its self into station mode

- Station Mode
In station mode the weather station will a webserver that can be accessed using a web browser. From the web browser the environmental data collected can be viewed.
