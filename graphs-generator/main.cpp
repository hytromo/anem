#include <QtCore/QCoreApplication>
#include <QSocketNotifier>
#include <QTimer>
#include <QDebug>
#include <QStringList>
#include <QTime>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>

#include <sys/termios.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <cmath>

using namespace std;

//GLOBAL VARIABLES
QString aurora_status="";
QString last_aurora_error="";
//commas number
const int commas_n = 12;
//for opening the device
int m_fd;
struct termios m_oldtio;
char m_buf[4096];

//for reading from the device
QString device="/dev/ttyUSB0";
QSocketNotifier *m_notifier;
QString output_buffer="";
int baudrate=9600;

//for calculating the average & everything that has to be sent to RRD...
int average_timeout=300;//every this timeout the average will be sent to rrdtool (seconds)
bool first_good_packet=true;
int packets_count=0;
bool average_just_sent=false;

//for the interpolation (when bad packets come, then (e.g) speed_avg for them is (old_speed+new_speed)/2
//giving some values to the old_* variables in case the 1st packet is wrong!

//for creating the graph
QString database_directory="/home/alex/ANEMOS_DATABASE/"; //<- please the database_directory to always end with '/'
QString graph_directory="/home/alex/ANEMOS_DATABASE/"; //<- please the graph_directory to always end with '/'
QString database_filename="speed.rrd";
QString speed_image="speed.png";
QString wind_image="wind.png";
QString acceleration_image="acceleration.png";
QString rpm_image="rpm.png";
QString watt_image="watt.png";

QString time_zone="Greek";
QString where="Chania-PETROGAZ";
//for feeding the graph with it...
bool dirty_flag=false;
bool bad_flag=false;
//for writing to the log file
QTime local_time;
QString log_directory="/home/alex/ANEMOS_LOG_FILES/"; //<- please the log_directory to always end with '/'
QString header="";//<- the header of every new file (changing daily)
QString current_log_file="";
QString old_log_file="";
bool log_file_changed=true;
QFile logfile;

QTextStream out;

//for totals
QFile totfile;
float totals[6]={0,0,0,0,0,0};          // Energy totals
int old_day, old_month, old_year;
int total_watts = 0;
float power_threshold = 19.0;           // Power output is summed if it is above this threshold

//other
QString application_dir="";
bool first_timer_run=true;

//variables for last, max and sum...
int last_second=0;
float last_wind_speed=0;
float last_wind_dir=0;
float last_acceleration=0;
float last_acceleration_max=0;
float last_rpm=0; //Rounds Per Sec
int last_DC_in1=0; //inverter DC input 1
int last_watt_out=0; //to DEH

int old_second=0;
float old_wind_speed=0;
float old_wind_dir=0;
float old_acceleration=0;
float old_acceleration_max=0;
float old_rpm=0;
int old_DC_in1=0; //inverter DC input 1
int old_watt_out=0; //to DEH

float wind_speed_sum=0;
float acceleration_sum=0;
float acceleration_max_sum=0;
float rpm_sum=0;
float wind_speed_max=0;
float max_rpm=0;
int max_watts_out=0;
int DC_in1_sum=0;
int watt_out_sum=0;

float dir_mod=0;
float wind_dir_sum=0;
float dir_stdev_sum=0;

//FUNCTIONS
char convert(char);
char calc_nmea(QString);
void open_device(QString);
void set_current_log_file();
int second_of_day();
int seconds_from_1970();
void write_to_log(int, float, float, float, float, float, int, int, QString, QString);
void read_config();
void create_config();
void create_databases();
int seconds_for_fifth_minute();
QString full_date();
void daychange(void);

class Timer : public QTimer {
  Q_OBJECT
public:
  explicit Timer(QObject *parent = 0) : QTimer(parent) {

  }
public slots:
    void readData(int fd){
        if (fd!=m_fd)
           return;

        int bytesRead=read(m_fd, m_buf, 4096);

        if (bytesRead<0)
        {
           cerr << "read result: " << bytesRead << endl;
           return;
        }
        // if the device "disappeared", e.g. from USB, we get a read event for 0 bytes
        else if (bytesRead==0)
        {
           disconnectTTY();
           return;
        }

        const char* c=m_buf;

        QString text="";
        for (int i=0; i<bytesRead; i++)
        {
            if ((isprint(*c)) || (*c=='\n') || (*c=='\r'))
            {
                text+=(*c);
            }
            else
            {
                char buf[64];
                unsigned int b=*c;
                snprintf(buf, 16, "\\0x%02x", b & 0xff);
                text+=buf;
            }
         c++;
        }
        addOutput(text);
    }
    void addOutput(QString text){
        output_buffer+=text;
        this->setSingleShot(true);
        this->start(100);
    }
    void process_input(){
        /*
           This function processes the input strings:
           1) Check if packet is good
           2) Make new_values->current_values
           3) Interpolate (the old_values have not been updated, so interpolation is possible)
           4) Make old_values->current_values
        */
        //An example input string: !WT,5.0,5.7,0.30,2.34,,,201.3,4211,4027,F1,E9,*20
        QString input=output_buffer;
        output_buffer.clear();

        if(input.right(1)!="\n")
            input+="\n";

        /*
          CHECK IF PACKET IS GOOD
        */

        if(input.count(",", Qt::CaseInsensitive)!=commas_n){
            action_at_bad_packet();
            return;
        }
        QStringList values=input.split(",");
        if(values.count()!=(commas_n+1)){
            action_at_bad_packet();
            return;
        }
        QString hash=values.at(commas_n);
        //checking if it contains the * character
        if(!hash.contains("*")){
            action_at_bad_packet();
            return;
        }
        hash.replace("*","");
        QChar qalpha=hash.at(0);
        char alpha=qalpha.toLatin1();
        QChar qbeta=hash.at(1);
        char beta=qbeta.toLatin1();
        char c_i_got=(convert(alpha) <<4)|convert(beta);
        char c_i_calced=calc_nmea(input);
        if(c_i_got!=c_i_calced){
            //hash is wrong, packet is wrong
            action_at_bad_packet();
            return;
        }

        /*
          If the code has reached this point, then the packet is GOOD!

          PROCESS VALUES
        */

        //extracting the values from the good packet:
        last_second=second_of_day();
        last_wind_speed=values.at(1).toFloat();
        last_wind_dir=values.at(2).toFloat();
        last_acceleration=values.at(3).toFloat();
        last_acceleration_max=values.at(4).toFloat();
        //5, 6 are empty for future use
        if(values.at(7).isEmpty()){
            //no data from the aurora which means that no power is being produced. Zero values to all of them.
            last_rpm=last_DC_in1=last_watt_out=0;
            last_aurora_error=aurora_status="00";
        }
        else
        {
            last_rpm=values.at(7).toFloat();
            last_DC_in1=values.at(8).toFloat();
            last_watt_out=values.at(9).toFloat();
            aurora_status=values.at(10);
            last_aurora_error=values.at(11);
        }



        if(!first_good_packet){
            //interpolation is possible, getting the second(s) difference between this and the last good packet
            int sec_diff=last_second-old_second;
            if(sec_diff<0)
                sec_diff+=86400;

            if((sec_diff>60)||(sec_diff==0)){
                //whoa! unusally lots of lost packets! discard them, or two packets in less than a second
                old_second=last_second;
                return;
            }

            float wind_speed_step=(last_wind_speed-old_wind_speed)/sec_diff;
            float wind_dir_diff=last_wind_dir-old_wind_dir;
            float acceleration_step=(last_acceleration-old_acceleration)/sec_diff;
            float acceleration_max_step=(last_acceleration_max-old_acceleration_max)/sec_diff;
            float rpm_step=(last_rpm-old_rpm)/sec_diff;
            float DC_in1_step=(last_DC_in1-old_DC_in1)/sec_diff;
            float watt_out_step=(last_watt_out-old_watt_out)/sec_diff;

            if(wind_dir_diff<-180){
                wind_dir_diff+=360;
            }
            else if(wind_dir_diff>=180)
            {
                wind_dir_diff-=360;
            }
            float wind_dir_step=wind_dir_diff/sec_diff;

            //looping between the packets, if there's no bad packet, the loop will run only once, otherwise interpolation takes place:
            for(int i=1;i<=sec_diff;i++){
                int sec_to_write=last_second-sec_diff+i;
                float wind_speed_to_write=old_wind_speed+wind_speed_step*i;
                float wind_dir_to_write=old_wind_dir+wind_dir_step*i;
                float acceleration_to_write=old_acceleration+acceleration_step*i;
                float acceleration_max_to_write=old_acceleration_max+acceleration_max_step*i;
                float rpm_to_write=old_rpm+rpm_step*i;
                float DC_in1_to_write=old_DC_in1+DC_in1_step*i;
                float watt_out_to_write=old_watt_out+watt_out_step*i;


                if(sec_to_write>86399)
                    sec_to_write-=86400;
                write_to_log(sec_to_write, wind_speed_to_write, wind_dir_to_write, acceleration_to_write, acceleration_max_to_write, rpm_to_write, DC_in1_to_write, watt_out_to_write, aurora_status, last_aurora_error);
                while(bad_flag){
                    usleep(10000);
                }

                dirty_flag=true;

                packets_count++;
                wind_speed_sum+=wind_speed_to_write;
                if(wind_speed_to_write>wind_speed_max)
                    wind_speed_max=wind_speed_to_write;
                if(watt_out_to_write>max_watts_out)
                    max_watts_out=watt_out_to_write;
                if(rpm_to_write>max_rpm)
                    max_rpm=rpm_to_write;
                if(average_just_sent){
                    average_just_sent=false;
                    dir_mod=wind_dir_to_write;
                }
                else
                {
                    dir_mod+=wind_dir_step;
                }
                wind_dir_sum+=dir_mod;
                dir_stdev_sum+=dir_mod*dir_mod;
                acceleration_sum+=acceleration_to_write;
                acceleration_max_sum+=acceleration_max_to_write;
                rpm_sum+=rpm_to_write;
                DC_in1_sum+=DC_in1_to_write;
                watt_out_sum+=watt_out_to_write;
                if(watt_out_to_write > power_threshold) total_watts+=watt_out_to_write;
                dirty_flag=false;
            }
        }
        else
        {
            //the old values will become the current, and, at the next good packet interpolation will be possible
            first_good_packet=false;
        }

        old_second=second_of_day();
        old_wind_speed=last_wind_speed;
        old_wind_dir=last_wind_dir;
        old_acceleration=last_acceleration;
        old_acceleration_max=last_acceleration_max;
        old_rpm=last_rpm;
        old_DC_in1=last_DC_in1;
        old_watt_out=last_watt_out;

        cout << "Good packet: " << QString(input).toLocal8Bit().data();
    }
    void give_avg(){
        /*
        This function is called so as to give the avg to rrd and create the graphs.
        It also clears the values so as to recalculate the next average_timeout seconds.
        */
        if(first_timer_run){
            //the fifth minute has come, now set it to the normal timeout
            this->stop();
            this->start(average_timeout*1000);
            first_timer_run=false;
        }

        if(!packets_count)
            return;

        //when the average is being sent...
        local_time=QDateTime::currentDateTime().time();
        QString hour,minute,second;
        int nhour=local_time.hour(),nminute=local_time.minute(),nsecond=local_time.second();
        if(nsecond<10)
            second="0"+QString::number(nsecond);
        else
            second=QString::number(nsecond);
        if(nminute<10)
            minute="0"+QString::number(nminute);
        else
            minute=QString::number(nminute);
        if(nhour<10)
            hour="0"+QString::number(nhour);
        else
            hour=QString::number(nhour);
        QString generated_at=" - "+QString::number(QDateTime::currentDateTime().date().day())+" "+QDate::shortMonthName(QDateTime::currentDateTime().date().month())+       " "+QString::number(QDateTime::currentDateTime().date().year())+        " at "+hour+":"+minute+":"+second+" ("+time_zone+" time)";

        while(dirty_flag)
            usleep(10000);

        /*MAKING THE AVERAGES*/

        bad_flag=true;

        float wind_speed_average=wind_speed_sum/packets_count;
        float wind_speed_maximum=wind_speed_max;
        float maximum_watts_out=max_watts_out;
        float maximum_rpm=max_rpm;
        float wind_dir_average=wind_dir_sum/packets_count;
        float dir_stdev=0, uroot=(dir_stdev_sum/packets_count)-wind_dir_average*wind_dir_average;
        if(uroot>0)
            dir_stdev=sqrt(uroot);
        float acceleration_average=acceleration_sum/packets_count;
        float acceleration_max_average=acceleration_max_sum/packets_count;
        float rpm_average=rpm_sum/packets_count;
        float DC_in1_average=((float)DC_in1_sum)/packets_count;
        float watt_out_average=((float)watt_out_sum)/packets_count;
        float temp=total_watts/3600000.0;

        for(int i=0;i<3;i++) {
            totals[i] += temp;
            totfile.seek(20*i);
            totfile.write(QString::number(totals[i],'f',3).toAscii()+"               ",19);
        }
        totfile.flush();
        int j = 1;
        if(totals[0]<10) j = 2;
        QString dayp = QString::number(totals[0],'f',j);

        packets_count=wind_speed_sum=max_rpm=wind_speed_max=max_watts_out=wind_dir_sum=dir_stdev_sum=acceleration_sum=acceleration_max_sum=rpm_sum=DC_in1_sum=watt_out_sum=total_watts=0;
        average_just_sent=true;

        bad_flag=false;

        /*ALTERING SOME VALUES*/
        if(wind_dir_average>=0){
            if(wind_dir_average>=360)
                    wind_dir_average-=360;
        }
        else
        {
            wind_dir_average+=360;
        }

        if(wind_speed_maximum > wind_speed_average*2)
            wind_speed_maximum=wind_speed_average*2; //to be removed if the gotten data about this are to be correct

        /*CREATING THE UPDATE COMMAND*/

        //Giving the average to RRDTOOL
        QString update_cmd="rrdtool update \""+database_directory+database_filename+"\" --template speed:maxspeed:direction:stdev:acceleration:acceleration_max:rpm:max_rpm:DC_in1:watt_out:max_watt_out N:"+QString::number(wind_speed_average)+":"+QString::number(wind_speed_maximum)+":"+QString::number(wind_dir_average)+":"+QString::number(dir_stdev)+":"+QString::number(acceleration_average)+":"+QString::number(acceleration_max_average)+":"+QString::number(rpm_average)+":"+QString::number(maximum_rpm)+":"+QString::number(DC_in1_average)+":"+QString::number(watt_out_average)+":"+QString::number(maximum_watts_out)+";";

        /*CREATING THE GRAPH COMMANDS*/

        int end_at=seconds_from_1970();
        int start_at=end_at-86400;//one day back
        QString last_average_speed_string=QString::number(wind_speed_average, 'f', 1);
        //speed graph (speed + max speed in one graph)
        QString graph_cmd1="rrdtool graph \""+graph_directory+speed_image+"\" --disable-rrdtool-tag --grid-dash 1:0 --x-grid MINUTE:30:HOUR:1:HOUR:6:0:\"%a %H:%M\" --units-length 5 -l 0 -n TITLE:20:Serif -n WATERMARK:10:Times -n AXIS:11:Times -n UNIT:14:DEFAULT -n LEGEND:10:DEFAULT -c GRID#32A60E90 -c MGRID#32A60E -h 300 -w 600 -t \"Wind Speed\" -v \"m/s\" -W \""+where+generated_at+"\" --start "+QString::number(start_at)+" --end "+QString::number(end_at)+" DEF:myspeed=\""+database_directory+database_filename+"\":speed:LAST DEF:mymaxspeed=\""+database_directory+database_filename+"\":maxspeed:LAST AREA:mymaxspeed#0000FF:\"Max Speed\" AREA:myspeed#FF0000:\"Average Speed\\l\" COMMENT:\"\\u\" COMMENT:\"Last speed\\:"+last_average_speed_string+" m/s\\r\";";
        //wind direction graph
        QString graph_cmd2="rrdtool graph \""+graph_directory+wind_image+"\" --disable-rrdtool-tag --grid-dash 1:0 --x-grid MINUTE:30:HOUR:1:HOUR:6:0:\"%a %H:%M\" --units-length 5 --y-grid 10:9 -l 0 -u 360 -r -n TITLE:20:Serif -n WATERMARK:10:Times -n AXIS:11:Times -n UNIT:14:DEFAULT -n LEGEND:10:DEFAULT -c GRID#ff000070 -c MGRID#ff000090 -h 300 -w 600 -t \"Wind Direction\" -v \"degrees\" -W \""+where+generated_at+"\" --start "+QString::number(start_at)+" --end "+QString::number(end_at)+" DEF:mydirection=\""+database_directory+database_filename+"\":direction:LAST DEF:mystdev=\""+database_directory+database_filename+"\":stdev:LAST AREA:mydirection#55A9FE:\"Average Direction\" AREA:mystdev#0D518B80:\"Standard Deviation\" LINE2:mydirection#3D83C9 LINE2:mystdev#0B3861;";
        //wind direction now graph
        QString graph_cmd3="convert -rotate "+QString::number(wind_dir_average)+" -crop 163x163+0+0 needle.bmp temp.bmp; composite temp.bmp wind_direction.bmp /var/www/images/wind_now.png;";
        //mast acceleration graph
        QString graph_cmd4="rrdtool graph \""+graph_directory+acceleration_image+"\" --disable-rrdtool-tag --grid-dash 1:0 --x-grid MINUTE:30:HOUR:1:HOUR:6:0:\"%a %H:%M\" --units-length 5 -l 0 -n TITLE:20:Serif -n WATERMARK:10:Times -n AXIS:11:Times -n UNIT:14:DEFAULT -n LEGEND:10:DEFAULT -c GRID#ff000070 -c MGRID#ff000090 -h 300 -w 600 -t \"Mast Acceleration\" -v \"g\" -W \""+where+generated_at+"\" --start "+QString::number(start_at)+" --end "+QString::number(end_at)+" DEF:myacceleration=\""+database_directory+database_filename+"\":acceleration:LAST DEF:myacceleration_max=\""+database_directory+database_filename+"\":acceleration_max:LAST AREA:myacceleration_max#ccff66:\"Peak value\" LINE0.5:myacceleration_max#669900 AREA:myacceleration#669900:\"RMS value\";";
        //rpm graph
        QString graph_cmd5="rrdtool graph \""+graph_directory+rpm_image+"\" --disable-rrdtool-tag --grid-dash 1:0 --x-grid MINUTE:30:HOUR:1:HOUR:6:0:\"%a %H:%M\" --units-length 5 -l 0 -n TITLE:20:Serif -n WATERMARK:10:Times -n AXIS:11:Times -n UNIT:14:DEFAULT -n LEGEND:10:DEFAULT -c GRID#32A60E90 -c MGRID#32A60E -h 300 -w 600 -t \"Wind Turbine Speed\" -v \"RPM\" -W \""+where+generated_at+"\" --start "+QString::number(start_at)+" --end "+QString::number(end_at)+" DEF:myrpm=\""+database_directory+database_filename+"\":rpm:LAST DEF:mymax_rpm=\""+database_directory+database_filename+"\":max_rpm:LAST AREA:mymax_rpm#ff897c:\"Max Speed\" AREA:myrpm#9fa4ee:\"Average Speed\" LINE1.5:mymax_rpm#8f4d46 LINE2:myrpm#54577e;";
        //DC_in1 and watt_out graph
//        QString graph_cmd6="rrdtool graph \""+graph_directory+watt_image+"\" --disable-rrdtool-tag --grid-dash 1:0 --x-grid MINUTE:30:HOUR:1:HOUR:6:0:\"%a %H:%M\" --units-length 5 -l 0 -n TITLE:20:Serif -n WATERMARK:10:Times -n AXIS:11:Times -n UNIT:14:DEFAULT -n LEGEND:10:DEFAULT -c GRID#0000ff70 -c MGRID#0000ff90 -h 300 -w 600 -t \"Power\" -v \"Watts\" -W \""+where+generated_at+"\" --start "+QString::number(start_at)+" --end "+QString::number(end_at)+" DEF:mywatt_out=\""+database_directory+database_filename+"\":watt_out:LAST DEF:myDC_in1=\""+database_directory+database_filename+"\":DC_in1:LAST DEF:mymaximum_watts_out=\""+database_directory+database_filename+"\":max_watt_out:LAST AREA:mymaximum_watts_out#20b2aa:\"Peak Power Out\" AREA:myDC_in1#ffb6c1:\"Average Power In\" AREA:mywatt_out#c71585:\"Average Power Out\\l\" LINE1.5:mymaximum_watts_out#136b66 LINE2:myDC_in1#7c595e COMMENT:\"\\u\" COMMENT:\"Day Total\\:"+dayp+" KWh\\r\";";
        QString graph_cmd6="rrdtool graph \""+graph_directory+watt_image+"\" --disable-rrdtool-tag --grid-dash 1:0 --x-grid MINUTE:30:HOUR:1:HOUR:6:0:\"%a %H:%M\" --units-length 5 -l 0 -n TITLE:20:Serif -n WATERMARK:10:Times -n AXIS:11:Times -n UNIT:14:DEFAULT -n LEGEND:10:DEFAULT -c GRID#0000ff70 -c MGRID#0000ff90 -h 300 -w 600 -t \"Power\" -v \"Watts\" -W \""+where+generated_at+"\" --start "+QString::number(start_at)+" --end "+QString::number(end_at)+" DEF:mywatt_out=\""+database_directory+database_filename+"\":watt_out:LAST DEF:mymaximum_watts_out=\""+database_directory+database_filename+"\":max_watt_out:LAST AREA:mymaximum_watts_out#20b2aa:\"Peak Power Out\" AREA:mywatt_out#ff1baa:\"Average Power Out\\l\" LINE1.5:mymaximum_watts_out#136b66 LINE1.5:mywatt_out#8a0f5c COMMENT:\"\\u\" COMMENT:\"Day Total\\:"+dayp+" KWh\\r\";";

        qDebug() << graph_cmd1 << endl;
        qDebug() << graph_cmd2 << endl;
        qDebug() << graph_cmd3 << endl;
        qDebug() << graph_cmd4 << endl;
        qDebug() << graph_cmd5 << endl;
        qDebug() << graph_cmd6 << endl;

        /*MAKING FINAL COMMAND*/
        QString final_cmd="bash -c '"+update_cmd+"sleep 2;"+graph_cmd1+graph_cmd2+graph_cmd3+graph_cmd4+graph_cmd5+graph_cmd6+"'&";
        /*EXECUTING...*/
        if(system((final_cmd.toLocal8Bit().data()))) cerr << "Error at final command\n";

    }
    void action_at_bad_packet(){
        cerr << "BAD packet!\n";
    }
    void disconnectTTY(){
        delete m_notifier;
        m_notifier=0;
    }
    void connect_normal_timer()
    {
        connect(this, SIGNAL(timeout()), this, SLOT(process_input()));
    }
    void connect_avg_maker_timer()
    {
        connect(this ,SIGNAL(timeout()), this, SLOT(give_avg()));
    }
    void disconnect_from_slot()
    {
        disconnect(this, 0, this, 0);
    }
    void create_notifier(){
        m_notifier=new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        connect(m_notifier, SIGNAL(activated(int)), this, SLOT(readData(int)));
    }
};

//timer that appends the input strings to form the packets
Timer timer;
//timer that sends the average to RRD and creates the graph every 5 minutes
Timer avg_maker;

#include "main.moc"


int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    //checking if the user is root
    if (getuid())
    {
        cerr << "You have to be root in order to execute this program!\nThe program needs to update system directories and files.\n";
        exit(1);
    }

    application_dir=QCoreApplication::applicationDirPath();

    if(QCoreApplication::arguments().count()>1){
        if(QCoreApplication::arguments().at(1)=="--config"){
            cout << "Configuration: Please answer the following questions, Give Ctrl+C for cancel any time:\n";
            create_config();
            exit(0);
        }
        else
        {
            cerr << QString("Argument '"+QCoreApplication::arguments().at(1)+"' not recognized!\nPossible arguments: --config\n").toLocal8Bit().data();
            exit(1);
        }
    }

    /*
      READ THE CONFIGURATION FILE anem.conf
    */

    read_config();

    /*
      SET CURRENT LOG FILE (this is needed so as the old_log_file to be initialized)
    */

    set_current_log_file();

    /*
      OPEN DEVICE
    */

    open_device(device);
    //set the output stream to the logfile
    out.setDevice(&logfile);

    /*
      CREATE DATABASE
    */

    create_databases();

    /*
      READ TOTALS
    */
    totfile.setFileName(QString(graph_directory+"totals.txt"));
    if(totfile.exists()) {
        if(!totfile.open(QIODevice::ReadWrite)) {
            cerr << "Error opening Total energy file" << endl;
            exit(1);
        }
        QFileInfo totfinfo(totfile.fileName());
        QTextStream in(&totfile);
        for(int i=0;i<6;i++) in >> totals[i];
        old_day = totfinfo.lastModified().toString("d").toInt();
        old_month = totfinfo.lastModified().toString("M").toInt();
        old_year = totfinfo.lastModified().toString("yyyy").toInt();
        if(QDateTime::currentDateTime().toString("ddMMyyyy")!=totfinfo.lastModified().toString("ddMMyyyy"))
            daychange();
    }
    else {
        if(!totfile.open(QIODevice::ReadWrite)) {
            cerr << "Error opening Total energy file" << endl;
            exit(1);
        }
        old_day = QDateTime::currentDateTime().toString("d").toInt();
        old_month = QDateTime::currentDateTime().toString("M").toInt();
        old_year = QDateTime::currentDateTime().toString("yyyy").toInt();
        for(int i=0;i<6;i++) {
            totfile.seek(i*20);
            totfile.write("0.000              \n",20);
        }
        totfile.flush();
    }

    /*
      SET UP TIMERS
    */

    timer.connect_normal_timer();
    avg_maker.connect_avg_maker_timer();

    /*
      WAIT FOR THE 5TH MINUTE TO COME & START THE AVERAGE MAKER TIMER
    */

    int seconds_for_fifth=seconds_for_fifth_minute();
    if(seconds_for_fifth){
        qDebug() << "Setting timer's timeout at" << seconds_for_fifth << "seconds... The next timeout will be " << average_timeout << " seconds\n";
        //if we have to wait for the 5th minute to come (01:00, 01:25 etc) then create the average for these seconds...
        avg_maker.start(seconds_for_fifth*1000);
    }
    else
    {
        avg_maker.start(average_timeout*1000);
    }

    /*
      CREATE SOCKET READER
    */
    timer.create_notifier();

    return a.exec();
}

void open_device(QString device){
    m_fd=open(device.toLatin1(), O_RDONLY | O_NDELAY);
    if(m_fd<0){
        cerr << QString("Cannot open device '"+device+"'. Is it plugged in?\n").toLocal8Bit().data();
        exit(1);
    }
   tcflush(m_fd, TCIOFLUSH);
   int n = fcntl(m_fd, F_GETFL, 0);
   fcntl(m_fd, F_SETFL, n & ~O_NDELAY);
   if (tcgetattr(m_fd, &m_oldtio)!=0)
   {
      cerr << "tcgetattr() 2 failed" << endl;
   }

   struct termios newtio;

   if (tcgetattr(m_fd, &newtio)!=0)
   {
      cerr << "tcgetattr() 3 failed" << endl;
   }

   speed_t _baud=0;
   switch (baudrate)
   {
#ifdef B0
   case      0: _baud=B0;     break;
#endif

#ifdef B50
   case     50: _baud=B50;    break;
#endif
#ifdef B75
   case     75: _baud=B75;    break;
#endif
#ifdef B110
   case    110: _baud=B110;   break;
#endif
#ifdef B134
   case    134: _baud=B134;   break;
#endif
#ifdef B150
   case    150: _baud=B150;   break;
#endif
#ifdef B200
   case    200: _baud=B200;   break;
#endif
#ifdef B300
   case    300: _baud=B300;   break;
#endif
#ifdef B600
   case    600: _baud=B600;   break;
#endif
#ifdef B1200
   case   1200: _baud=B1200;  break;
#endif
#ifdef B1800
   case   1800: _baud=B1800;  break;
#endif
#ifdef B2400
   case   2400: _baud=B2400;  break;
#endif
#ifdef B4800
   case   4800: _baud=B4800;  break;
#endif
#ifdef B7200
   case   7200: _baud=B7200;  break;
#endif
#ifdef B9600
   case   9600: _baud=B9600;  break;
#endif
#ifdef B14400
   case  14400: _baud=B14400; break;
#endif
#ifdef B19200
   case  19200: _baud=B19200; break;
#endif
#ifdef B28800
   case  28800: _baud=B28800; break;
#endif
#ifdef B38400
   case  38400: _baud=B38400; break;
#endif
#ifdef B57600
   case  57600: _baud=B57600; break;
#endif
#ifdef B76800
   case  76800: _baud=B76800; break;
#endif
#ifdef B115200
   case 115200: _baud=B115200; break;
#endif
#ifdef B128000
   case 128000: _baud=B128000; break;
#endif
#ifdef B230400
   case 230400: _baud=B230400; break;
#endif
#ifdef B460800
   case 460800: _baud=B460800; break;
#endif
#ifdef B576000
   case 576000: _baud=B576000; break;
#endif
#ifdef B921600
   case 921600: _baud=B921600; break;
#endif
   }
   cfsetospeed(&newtio, (speed_t)_baud);
   cfsetispeed(&newtio, (speed_t)_baud);

   newtio.c_cflag = (newtio.c_cflag & ~CSIZE) | CS8;
   newtio.c_cflag |= CLOCAL | CREAD;

   newtio.c_cflag &= ~(PARENB | PARODD);

   newtio.c_cflag &= ~CRTSCTS;

   newtio.c_cflag &= ~CSTOPB;

   newtio.c_iflag=IGNBRK;

   newtio.c_iflag &= ~(IXON|IXOFF|IXANY);

   newtio.c_lflag=0;
   newtio.c_oflag=0;

   newtio.c_cc[VTIME]=1;
   newtio.c_cc[VMIN]=60;

   if (tcsetattr(m_fd, TCSANOW, &newtio)!=0)
   {
      cerr << "tcsetattr() 1 failed" << endl;
   }

   int mcs=0;
   ioctl(m_fd, TIOCMGET, &mcs);
   mcs |= TIOCM_RTS;
   ioctl(m_fd, TIOCMSET, &mcs);

   if (tcgetattr(m_fd, &newtio)!=0)
   {
      cerr << "tcgetattr() 4 failed" << endl;
   }

   newtio.c_cflag &= ~CRTSCTS;

   if (tcsetattr(m_fd, TCSANOW, &newtio)!=0)
   {
      cerr << "tcsetattr() 2 failed" << endl;
   }
}

char convert(char in){
    if(in < 0x3A)
        return in-0x30;
    else
        return in-0x37;
}

char calc_nmea(QString input_string){
    //input_string is a correct input_string we've got. just chop it
    int count=input_string.count();
    if(!count){
        return 0;
    }
    //checking how many times contains the $ character
    int asterisk_count=0;
    int asterisk_pos=0;
    for(int i=0;i<count;i++){
        if(input_string.at(i)=='*'){
            asterisk_count++;
            asterisk_pos=i;
        }
    }
    if(asterisk_count!=1){
        return 0;
    }
    if(input_string.at(0)!='!'){
        return 0;
    }
    //here, input_string contains one $ at 1st pos + one *
    if(asterisk_pos+2>count){
        //there are not enough characters after the asterisk...
        return 0;
    }
    //here, input_string contains one $ at 1st pos + one * with enough characters after it
    QString final;
    for(int i=1;i<asterisk_pos;i++){
        final+=input_string.at(i);
    }
    char *cfinal=final.toLocal8Bit().data();
    char nmea=0;
    int count1=final.count();
    for(int i=0;i<count1;i++){
        nmea^=cfinal[i];
    }
    return nmea;
}

int second_of_day(){
    local_time=QDateTime::currentDateTime().time();
    return local_time.hour()*3600+local_time.minute()*60+local_time.second();
}

int seconds_from_1970(){
    return time(NULL);
}

QString full_date(){
    return QDate::currentDate().toString();
}

void set_current_log_file(){
    //01 (Jan) -> 01

    int day_of_month_n=QDateTime::currentDateTime().date().day();
    QString day_of_month;
    if(day_of_month_n<10)
        day_of_month="0"+QString::number(day_of_month_n);
    else
        day_of_month=QString::number(day_of_month_n);

    int month_n=QDateTime::currentDateTime().date().month();
    QString month;
    if(month_n<10)
        month="0"+QString::number(month_n);
    else
        month=QString::number(month_n);

    QString month_name=QDate::shortMonthName(month_n);
    current_log_file=log_directory+month+" ("+month_name+")/"+day_of_month;
    if(old_log_file!=current_log_file)
    {
        if(first_good_packet){
            //the function was called from inside the main() function, initialize the old_log_file
            //creating directory...
            if(system(QString("mkdir -p \""+log_directory+month+" ("+month_name+")/\"").toLocal8Bit().data()))
                cerr << "Probably could not create a directory for logging. Please check your permissions!\n";
            old_log_file=current_log_file;
            return;
        }
        //LOG FILE CHANGED, DO ACTIONS LIKE COMPRESSION HERE
        //clearing memory...
        //if (system("bash -c 'sync; echo 3 > /proc/sys/vm/drop_caches'&")) cerr << "Error cleaning memory\n";
        //make total power actions
        daychange();
        //creating directory...
        if(system(QString("mkdir -p \""+log_directory+month+" ("+month_name+")/\"").toLocal8Bit().data()))
            cerr << "Probably could not create a directory for logging. Please check your permissions!\n";

        if(QFile(current_log_file).exists()){
            //log file has already been created (in case of overwriting from previous year)
            QFile::remove(current_log_file);
        }
        old_log_file=current_log_file;
        log_file_changed=true;
    }
    else
    {
        log_file_changed=false;
    }
}

void write_to_log(int second, float speed, float wind, float acceleration, float acceleration_max, float rpm, int DC_in1, int watt_out, QString aurora_status, QString last_aurora_error){
    /*
      Function to write to the log file...
      Its arguments are:
      a) Second of the day
      b) Wind speed (m/s)
      c) Wind direction (degrees) ... etc
    */
    set_current_log_file();
    if(log_file_changed){
        if(logfile.isOpen())
            logfile.close();
        logfile.setFileName(current_log_file);
        logfile.open(QIODevice::Append);

        header=where+" ("+full_date()+")\n\nFormat:\nSecond of the day,Wind Speed,Wind Direction,Mast Acceleration RMS,Mast Acceleration Peak,Option 1,Option 2,RPM,Volts DC,Power Output,Status,Last Error,Inputs,Outputs\n\n";
        out << header;
        log_file_changed=false;
    }
    else
    {
        if(!logfile.isOpen()){
            logfile.setFileName(current_log_file);
            logfile.open(QIODevice::Append);
        }
    }
    if(wind<0)
        wind+=360;
    else if(wind>=360)
        wind-=360;
    out << second << "," << QString::number(speed, 'f', 1) << "," << QString::number(wind, 'f', 1) << "," << QString::number(acceleration, 'f', 2) << "," << QString::number(acceleration_max, 'f', 2)  << "," << "," << "," << QString::number(rpm, 'f', 1) << "," << QString::number(DC_in1) << "," << QString::number(watt_out) << "," << aurora_status << "," << last_aurora_error << "," << "00" << "," << "00" << endl;
}

void create_config(){
    QFile conf(application_dir+"/anem.conf");
    bool correct_info=false;
    QString y_n="n";
    QFile stdin_input;
    stdin_input.open(stdin, QIODevice::ReadOnly);
    QTextStream qt_in(&stdin_input);
    while(!correct_info){
        cout << "Device file (e.g. /dev/ttyUSB0): ";
        qt_in >> device;
        cout << "Baudrate: ";
        cin >> baudrate;
        cout << "Where to keep the log files?: ";
        qt_in >> log_directory;
        if(!log_directory.endsWith("/"))
            log_directory+="/";
        cout << "Where to create the RRDtool database? : ";
        qt_in >> database_directory;
        if(!database_directory.endsWith("/"))
            database_directory+="/";
        cout << "Database name : ";
        qt_in >> database_filename;
        cout << "Where to save the graphs? : ";
        qt_in >> graph_directory;
        if(!graph_directory.endsWith("/"))
            graph_directory+="/";
        cout << "Timeout in which the average to be given to rrdtool (secs) : ";
        cin >> average_timeout;
        cout << "Filename of speed graph (e.g. speed.png) : ";
        qt_in >> speed_image;
        cout << "Filename of wind direction graph : ";
        qt_in >> wind_image;
        cout << "Filename of acceleration graph : ";
        qt_in >> acceleration_image;
        cout << "Filename of rpm graph : ";
        qt_in >> rpm_image;
        cout << "Filename of watt graph : ";
        qt_in >> watt_image;
        //log directory, rrdtool database directory, graph_directory, Average timeout, graph timeout
        cout << "Timezone for use at the watermark (e.g. Greek or Athens or local) : ";
        qt_in >> time_zone;
        cout << "Location of the wind turbine and company (e.g. Chania-PETROGAZ (no space please)) : ";
        qt_in >> where;
        cout << "Are the above values you've set correct? (y/n)";
        qt_in >> y_n;
        if(y_n=="YES" || y_n=="yes" || y_n=="Y" || y_n=="y"){
            correct_info=true;
        }
        else
        {
            cout << "Please try again: \n";
        }
    }
    stdin_input.close();
    //generating the configuration file and writing to it the inputted values...
    if(conf.exists())
        QFile::remove(conf.fileName());
    conf.open(QIODevice::WriteOnly);
    QTextStream into_conf(&conf);
    into_conf << "Device = " << device << endl;
    into_conf << "Baudrate = " << baudrate << endl;
    into_conf << "Log_Dir = " << log_directory << endl;
    into_conf << "Database_Dir = " << database_directory << endl;
    into_conf << "Database_Name = " << database_filename << endl;
    into_conf << "Graph_Dir = " << graph_directory << endl;
    into_conf << "Avg_Timeout = " << average_timeout << endl;
    into_conf << "Speed_Graph = " << speed_image << endl;
    into_conf << "Wind_Graph = " << wind_image << endl;
    into_conf << "Acceleration_Graph = " << acceleration_image << endl;
    into_conf << "Rpm_graph = " << rpm_image << endl;
    into_conf << "Watt_graph = " << watt_image << endl;
    into_conf << "Watermark_timezone = " << time_zone << endl;
    into_conf << "Location = " << where << endl;
    into_conf << "Please change only the values and nothing else, not even the position of '='! The directories should end with /\n";
    conf.close();
    cout << "The configuration file has been created!\n";
}

QString form(QString input){
    //removes extra spaces from the front and end of 'input'
    while(input.startsWith(" ")){
        input.remove(0, 1);
    }
    while(input.endsWith(" ")){
        input.chop(1);
    }
    return input;
}

void read_config(){
    QFile conf(application_dir+"/anem.conf");
    if(!conf.exists()){
        cerr << "Configuration file anem.conf not found! Please create it via the --config argument!\n";
        exit(1);
    }
    else
    {
        //configuration file exists, load the values from there!
        conf.open(QIODevice::ReadOnly);
        QTextStream out_conf(&conf);
        try
        {
            device=form(out_conf.readLine().split("=")[1]);
            baudrate=form(out_conf.readLine().split("=")[1]).toInt();
            log_directory=form(out_conf.readLine().split("=")[1]);
            database_directory=form(out_conf.readLine().split("=")[1]);
            database_filename=form(out_conf.readLine().split("=")[1]);
            graph_directory=form(out_conf.readLine().split("=")[1]);
            average_timeout=form(out_conf.readLine().split("=")[1]).toInt();
            speed_image=form(out_conf.readLine().split("=")[1]);
            wind_image=form(out_conf.readLine().split("=")[1]);
            acceleration_image=form(out_conf.readLine().split("=")[1]);
            rpm_image=form(out_conf.readLine().split("=")[1]);
            watt_image=form(out_conf.readLine().split("=")[1]);
            time_zone=form(out_conf.readLine().split("=")[1]);
            where=form(out_conf.readLine().split("=")[1]);
        }
        catch (...)
        {
            cerr << "There is a problem with your configuration file! Please delete it and re-run the program with the --config argument!\n";
            exit(1);
        }
    }
    if(system(QString("mkdir -p \""+graph_directory+"\"").toLocal8Bit().data()))
            cerr << QString("Could not possibly create your graph directory '"+graph_directory+"'. Check your permissions\n").toLocal8Bit().data();
}

void create_databases(){
    if(system(QString("mkdir -p \""+database_directory+"\"").toLocal8Bit().data()))
        cerr << "Probably could not create a directory for logging. Please check your permissions!\n";
    if(!QFile(database_directory+database_filename).exists())
    {
        if(system(QString("rrdtool create \""+database_directory+database_filename+"\" --start N --step 300 DS:speed:GAUGE:1800:U:U DS:maxspeed:GAUGE:1800:U:U DS:direction:GAUGE:1800:U:U DS:stdev:GAUGE:1800:U:U DS:acceleration:GAUGE:1800:U:U DS:acceleration_max:GAUGE:1800:U:U DS:rpm:GAUGE:1800:U:U DS:max_rpm:GAUGE:1800:U:U DS:DC_in1:GAUGE:1800:U:U DS:watt_out:GAUGE:1800:U:U DS:max_watt_out:GAUGE:1800:U:U RRA:LAST:0.5:1:288").toLocal8Bit().data()))
            cerr << "Error while creating the rrdtool database!\n";
    }
}

int seconds_for_fifth_minute(){
    //this function returns how many seconds have to pass till the time is 00:05 || 00:10 || 00:15 etc...
    int passed=seconds_from_1970()%300;
    if(passed!=0)
        return 300-passed;
    else
        return 0;
}
void daychange(void) {
    int telos,day,year,month;
    day = QDateTime::currentDateTime().toString("d").toInt();
    month = QDateTime::currentDateTime().toString("M").toInt();
    year = QDateTime::currentDateTime().toString("yyyy").toInt();
    if(year!=old_year) telos=3;
    else if(month!=old_month) telos=2;
    else telos=1;
    for(int i=0;i<telos;i++) {
        totals[i+3] = totals[i];
        totfile.seek(20*(i+3));
        totfile.write(QString::number(totals[i+3],'f',3).toAscii()+"               ",19);
        totals[i]=0;
        totfile.seek(20*i);
        totfile.write("0.000              ",19);
    }
    totfile.flush();
    old_day=day; old_month=month; old_year=year;
    return;
}
