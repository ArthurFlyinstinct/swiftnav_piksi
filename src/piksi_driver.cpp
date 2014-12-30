#include "swiftnav_piksi/piksi_driver.h"

#include <libswiftnav/sbp.h>
#include <libswiftnav/sbp_messages.h>

#include <iomanip>

#include <sensor_msgs/NavSatFix.h>
#include <sensor_msgs/NavSatStatus.h>
#include <sensor_msgs/TimeReference.h>
#include <nav_msgs/Odometry.h>
#include <ros/time.h>
#include <tf/tf.h>

namespace swiftnav_piksi
{	
	PIKSI::PIKSI( const ros::NodeHandle &_nh,
		const ros::NodeHandle &_nh_priv,
		const std::string _port ) :
		nh( _nh ),
		nh_priv( _nh_priv ),
		port( _port ),
		frame_id( "gps" ),
		piksid( -1 ),

		min_update_rate( 20.0 ),
		max_update_rate( 80.0 ),
		min_rtk_rate( 0.5 ),
		max_rtk_rate( 10.0 ),

		piksi_pub_freq( diagnostic_updater::FrequencyStatusParam( &min_update_rate, &max_update_rate, 0.1, 10 ) ),
		rtk_pub_freq( diagnostic_updater::FrequencyStatusParam( &min_rtk_rate, &max_rtk_rate, 0.1, 10 ) ),

		io_failure_count( 0 ),
		open_failure_count( 0 ),
		spin_rate( 2000 ),      // call sbp_process this fast to avoid dropped msgs
		spin_thread( &PIKSI::spin, this )
	{
		cmd_lock.unlock( );
		diag.setHardwareID( "Swift_Nav_Piksi" );
		diag.add( "Swift Navigation Piksi Status", this, &PIKSI::DiagCB );
		diag.add( piksi_pub_freq );

		nh_priv.param( "frame_id", frame_id, (std::string)"gps" );
	}

	PIKSI::~PIKSI( )
	{
		spin_thread.interrupt( );
		PIKSIClose( );
	}

	bool PIKSI::PIKSIOpen( )
	{
		boost::mutex::scoped_lock lock( cmd_lock );
		return PIKSIOpenNoLock( );
	}

	bool PIKSI::PIKSIOpenNoLock( )
	{
		if( piksid >= 0 )
			return true;

		piksid = piksi_open( port.c_str( ) );

		if( piksid < 0 )
		{
			open_failure_count++;
			return false;
		}

		sbp_state_init(&state);
		sbp_state_set_io_context(&state, &piksid);

		sbp_register_callback(&state, SBP_HEARTBEAT, &heartbeatCallback, (void*) this, &heartbeat_callback_node);
		sbp_register_callback(&state, SBP_GPS_TIME, &timeCallback, (void*) this, &time_callback_node);
//		sbp_register_callback(&state, SBP_POS_ECEF, &pos_ecefCallback, (void*) this, &pos_ecef_callback_node);
		sbp_register_callback(&state, SBP_POS_LLH, &pos_llhCallback, (void*) this, &pos_llh_callback_node);
//		sbp_register_callback(&state, SBP_BASELINE_ECEF, &baseline_ecefCallback, (void*) this, &baseline_ecef_callback_node);
		sbp_register_callback(&state, SBP_BASELINE_NED, &baseline_nedCallback, (void*) this, &baseline_ned_callback_node);
//		sbp_register_callback(&state, SBP_VEL_ECEF, &vel_ecefCallback, (void*) this, &vel_ecef_callback_node);
//		sbp_register_callback(&state, SBP_VEL_NED, &vel_nedCallback, (void*) this, &vel_ned_callback_node);

		llh_pub = nh.advertise<sensor_msgs::NavSatFix>( "gps/fix", 1 );
		rtk_pub = nh.advertise<nav_msgs::Odometry>( "gps/rtkfix", 1 );
		time_pub = nh.advertise<sensor_msgs::TimeReference>( "gps/time", 1 );

		return true;
	}

	void PIKSI::PIKSIClose( )
	{
		boost::mutex::scoped_lock lock( cmd_lock );
		PIKSICloseNoLock( );
	}

	void PIKSI::PIKSICloseNoLock( )
	{
		int8_t old_piksid = piksid;
		if( piksid < 0 )
		{
			return;
		}
		piksid = -1;
		piksi_close( old_piksid );
		if( llh_pub )
			llh_pub.shutdown( );
		if( time_pub )
			time_pub.shutdown( );
	}

	void heartbeatCallback(u16 sender_id, u8 len, u8 msg[], void *context)
	{
		if ( context == NULL )
		{
			std::cerr << "Context void, OHSHIT" << std::endl;
			return;
		}
		
		sbp_heartbeat_t hb = *(sbp_heartbeat_t*) msg;

		class PIKSI *driver = (class PIKSI*) context;

		if (hb.flags & 1)
			std::cout << "an error has occured in a heartbeat message" << std::endl;
			
		return;
	}

	void timeCallback(u16 sender_id, u8 len, u8 msg[], void *context)
	{
		if ( context == NULL )
		{
			std::cerr << "Context void, OHSHIT" << std::endl;
			return;
		}

		class PIKSI *driver = (class PIKSI*) context;

		sbp_gps_time_t time = *(sbp_gps_time_t*) msg;

		sensor_msgs::TimeReferencePtr time_msg( new sensor_msgs::TimeReference );

		time_msg->header.frame_id = driver->frame_id;
		time_msg->header.stamp = ros::Time::now( );

		time_msg->time_ref.sec = time.tow;
		time_msg->source = "gps";

		driver->time_pub.publish( time_msg );
		driver->piksi_pub_freq.tick( );

		return;
	}

	void pos_llhCallback(u16 sender_id, u8 len, u8 msg[], void *context)
	{
		if ( context == NULL )
		{
			std::cerr << "Context void, OHSHIT" << std::endl;
			return;
		}

		class PIKSI *driver = (class PIKSI*) context;

		sbp_pos_llh_t llh = *(sbp_pos_llh_t*) msg;

		sensor_msgs::NavSatFixPtr llh_msg( new sensor_msgs::NavSatFix );

		llh_msg->header.frame_id = driver->frame_id;
		llh_msg->header.stamp = ros::Time::now( );

		llh_msg->status.status = 0;
		llh_msg->status.service = 1;

		llh_msg->latitude = llh.lat;
		llh_msg->longitude = llh.lon;
		llh_msg->altitude = llh.height;
        
        // FIXME: populate covariance. Find out how I know if I have a fix

		driver->llh_pub.publish( llh_msg );
		driver->piksi_pub_freq.tick( );

		return;
	}

/*	void baseline_ecef_callback(u16 sender_id, u8 len, u8 msg[], void *context)
	{

		if ( context == NULL )
		{
			std::cerr << "Context void, OHSHIT" << std::endl;
			return;
		}

		class PIKSI *driver = (class PIKSI*) context;

		sbp_gps_time_t time = *(sbp_gps_time_t*) msg;

		sensor_msgs::TimeReferencePtr time_msg( new sensor_msgs::TimeReference );

		time_msg->header.frame_id = driver->frame_id;
		time_msg->header.stamp = ros::Time::now( );

		time_msg->time_ref.sec = time.tow;
		time_msg->source = "gps";

		driver->time_pub.publish( time_msg );
		driver->piksi_pub_freq.tick( );

		return;
	}
*/
	void baseline_nedCallback(u16 sender_id, u8 len, u8 msg[], void *context)
	{

		if ( context == NULL )
		{
			std::cerr << "Context void, OHSHIT" << std::endl;
			return;
		}

		class PIKSI *driver = (class PIKSI*) context;

		sbp_baseline_ned_t rtk = *(sbp_baseline_ned_t*) msg;

		nav_msgs::OdometryPtr rtk_odom_msg( new nav_msgs::Odometry );

		rtk_odom_msg->header.frame_id = driver->frame_id;
        // For best accuracy, header.stamp should really get tow converted to ros::Time
		rtk_odom_msg->header.stamp = ros::Time::now( );

        // convert to meters from mm, and NED to ENU
		rtk_odom_msg->pose.pose.position.x = rtk.e/1000.0;
		rtk_odom_msg->pose.pose.position.y = rtk.n/1000.0;
		rtk_odom_msg->pose.pose.position.z = -rtk.d/1000.0;

        std::cout << "In baseline_nedCallback: (" << rtk.n/1000.0 << ", " << rtk.e/1000.0 << ")\n";

        float h_covariance = 1.0e3;
        float v_covariance = 1.0e3;

        // populate the pose covariance matrix if we have a good fix
        if ( 1 == rtk.flags && 4 < rtk.n_sats)
        {
            h_covariance = rtk.h_accuracy * rtk.h_accuracy;
            v_covariance = rtk.v_accuracy * rtk.v_accuracy;
        }
            
        rtk_odom_msg->pose.covariance[0]  = h_covariance;   // x = 0, 0 in the 6x6 cov matrix
        rtk_odom_msg->pose.covariance[7]  = h_covariance;   // y = 1, 1
        rtk_odom_msg->pose.covariance[14] = v_covariance;  // z = 2, 2

        // default rotational velocity to unknown
        rtk_odom_msg->pose.covariance[21] = 1.0e3;  // x rotation = 3, 3
        rtk_odom_msg->pose.covariance[28] = 1.0e3;  // y rotation = 4, 4
        rtk_odom_msg->pose.covariance[35] = 1.0e3;  // z rotation = 5, 5

        // set up the Twist covariance matrix - gps doesn't provide twist
        // Question: should I publish x, y, z velocity?
        rtk_odom_msg->pose.covariance[0]  = 1.0e3;   // x = 0, 0 in the 6x6 cov matrix
        rtk_odom_msg->pose.covariance[7]  = 1.0e3;   // y = 1, 1
        rtk_odom_msg->pose.covariance[14] = 1.0e3;  // z = 2, 2
        rtk_odom_msg->pose.covariance[21] = 1.0e3;  // x rotation = 3, 3
        rtk_odom_msg->pose.covariance[28] = 1.0e3;  // y rotation = 4, 4
        rtk_odom_msg->pose.covariance[35] = 1.0e3;  // z rotation = 5, 5


		driver->rtk_pub.publish( rtk_odom_msg );
		driver->piksi_pub_freq.tick( );

		return;
	}
/*
	void vel_ecef_callback(u16 sender_id, u8 len, u8 msg[], void *context)
	{
		if ( context == NULL )
		{
			std::cerr << "Context void, OHSHIT" << std::endl;
			return;
		}

		class PIKSI *driver = (class PIKSI*) context;

		sbp_gps_time_t time = *(sbp_gps_time_t*) msg;

		sensor_msgs::TimeReferencePtr time_msg( new sensor_msgs::TimeReference );

		time_msg->header.frame_id = driver->frame_id;
		time_msg->header.stamp = ros::Time::now( );

		time_msg->time_ref.sec = time.tow;
		time_msg->source = "gps";

		driver->time_pub.publish( time_msg );
		driver->piksi_pub_freq.tick( );

		return;
	}

	void vel_ned_callback(u16 sender_id, u8 len, u8 msg[], void *context)
	{
		if ( context == NULL )
		{
			std::cerr << "Context void, OHSHIT" << std::endl;
			return;
		}

		class PIKSI *driver = (class PIKSI*) context;

		sbp_gps_time_t time = *(sbp_gps_time_t*) msg;

		sensor_msgs::TimeReferencePtr time_msg( new sensor_msgs::TimeReference );

		time_msg->header.frame_id = driver->frame_id;
		time_msg->header.stamp = ros::Time::now( );

		time_msg->time_ref.sec = time.tow;
		time_msg->source = "gps";

		driver->time_pub.publish( time_msg );
		driver->piksi_pub_freq.tick( );

		return;
	}
*/
	void PIKSI::spin( )
	{
		while( ros::ok( ) )
		{
			boost::this_thread::interruption_point( );
			PIKSI::spinOnce( );
			diag.update( );
			spin_rate.sleep( );
		}
	}

	void PIKSI::spinOnce( )
	{
		int ret;

		cmd_lock.lock( );
		if( piksid < 0 && !PIKSIOpenNoLock( ) )
		{
			cmd_lock.unlock( );
			return;
		}

		ret = sbp_process( &state, &read_data );

        if(ret > 0)
            std::cout << ".";

		cmd_lock.unlock( );

	}

	void PIKSI::DiagCB( diagnostic_updater::DiagnosticStatusWrapper &stat )
	{
		boost::mutex::scoped_lock lock( cmd_lock );
		if( piksid < 0 && !PIKSIOpenNoLock( ) )
		{
			stat.summary( diagnostic_msgs::DiagnosticStatus::ERROR, "Disconnected" );
			return;
		}

		stat.summary( diagnostic_msgs::DiagnosticStatus::OK, "PIKSI status OK" );

		int ret;

		static unsigned int last_io_failure_count = io_failure_count;
		if( io_failure_count > last_io_failure_count )
			stat.summary( diagnostic_msgs::DiagnosticStatus::WARN, "I/O Failure Count Increase" );
		stat.add( "io_failure_count", io_failure_count );
		last_io_failure_count = io_failure_count;

		static unsigned int last_open_failure_count = open_failure_count;
		if( open_failure_count > last_open_failure_count )
			stat.summary( diagnostic_msgs::DiagnosticStatus::ERROR, "Open Failure Count Increase" );
		stat.add( "open_failure_count", open_failure_count );
		last_open_failure_count = open_failure_count;
	}

}
