#include "gonzales.h"
#include <stdio.h>
#include <sys/time.h>
#include <math.h>
#include "RosHelper.h"
#include "util.h"

#include <string.h>
#include <DateTime.h>

#define CONTROLLER_COUNT 4

using namespace msl_msgs;
using namespace msl_actuator_msgs;

struct timeval gonz_last_calltime;
struct timeval gonz_cur_calltime;
long lifeguardcounter;

double sinphi;
double cosphi;
double wheelcirc;
double robotcirc;
double finfactor;



struct timeval odotime_last;
struct timeval odotime_cur;
struct timeval lastOdoSendTime;

gonzales_state gonz_state;

int gonz_mode;

void gonz_init() {
    gonz_state.state = GONZ_RUNNING;
    gonz_state.autorecover = 0;
    gonz_state.recover_timer = 0;
    gettimeofday(&gonz_last_calltime,NULL);
    lifeguardcounter=0;

    gonz_update_derived_settings();

	gonz_set_mode(GONZ_MODE_NORMAL);

    gonz_reset();
    gonz_state.currentPosition.x = 0;
    gonz_state.currentPosition.y = 0;
    gonz_state.currentPosition.angle = 0;

}
void gonz_set_mode(int mode) {
	gonz_mode=mode;
}
int gonz_get_mode() {
	return gonz_mode;
}

void gonz_update_derived_settings() {
    sinphi = sin(current_settings.definingAngle);
    cosphi = cos(current_settings.definingAngle);
    wheelcirc = TWO_PI * current_settings.wheelRadius;
    robotcirc = TWO_PI * current_settings.robotRadius;
    finfactor = (double)current_settings.gear_ratio_denominator/(wheelcirc*(double)current_settings.gear_ratio_nominator)*60.0;
}
void gonz_main() { //main loop
        gettimeofday(&gonz_cur_calltime,NULL);
        gonz_calc_odometry();
        switch(gonz_state.state) {
            case GONZ_RUNNING: //normal op
                gonz_control();
            break;
            case GONZ_ERROR:

            if (gonz_state.autorecover) { //if not set, reboot is necessary
				ep->Trigger(-1);
				ep->ResetFaults();
           		long timediff =TIMEDIFFMS(gonz_cur_calltime,gonz_last_calltime);
               	gonz_state.recover_timer -= timediff;
                //printf("Timediff: %ld timer: %d\n",timediff,gonz_state.recover_timer);
           		if (gonz_state.recover_timer <= 0) {
      				printf("Recovering from error...\n");
               		gonz_recover_from_error();
               	}
            }
            break;
            case GONZ_INITIALISING: //dunno yet
            break;
            default:
                printf("SOMETHING gone haywire\n");
                gonz_set_error_state(1,GONZ_REST_ON_ERROR_MS);
                gonz_emergency_stop();
        }
        gonz_last_calltime = gonz_cur_calltime;
}
void gonz_control() { //controller comes in here

    ep->Trigger(1);

    if(!gonz_check_status()) {
        gonz_emergency_stop();
        return;
    }
    //Calculate Wheel Goal from Motion Goal:

        unsigned char i;

    gonz_state.currentMotorGoal[0] = -cosphi * gonz_state.currentMotionGoal.x;
    gonz_state.currentMotorGoal[0] += sinphi * gonz_state.currentMotionGoal.y;

    gonz_state.currentMotorGoal[1] = -cosphi * gonz_state.currentMotionGoal.x;
    gonz_state.currentMotorGoal[1] += -sinphi * gonz_state.currentMotionGoal.y;

    gonz_state.currentMotorGoal[2] = cosphi * gonz_state.currentMotionGoal.x;
    gonz_state.currentMotorGoal[2] += -sinphi * gonz_state.currentMotionGoal.y;

    gonz_state.currentMotorGoal[3] = cosphi * gonz_state.currentMotionGoal.x;
    gonz_state.currentMotorGoal[3] += sinphi * gonz_state.currentMotionGoal.y;

//printf("Vx: %d\t cospi %f\t finfactor: %f\n",gonz_state.currentMotionGoal.x,cosphi,finfactor);

    for (i=0; i < 4; i++) {
        gonz_state.currentMotorGoal[i] += (gonz_state.currentMotionGoal.rotation*current_settings.robotRadius)/1024.0;
        gonz_state.currentMotorGoal[i] *= finfactor;
    }
	if (current_settings.slipControlEnabled > 0  && fabs(gonz_state.currentSlip) > 1.0) {
		double setSlip = current_settings.slipControlP*gonz_state.currentSlip + current_settings.slipControlI*gonz_state.slipI;
		gonz_state.currentMotorGoal[0] -= setSlip;
		gonz_state.currentMotorGoal[1] += setSlip;
		gonz_state.currentMotorGoal[2] -= setSlip;
		gonz_state.currentMotorGoal[3] += setSlip;
//		printf("Reacting: p: %f\ti: %f\tt: %f\n",gonz_state.currentSlip,gonz_state.slipI,setSlip);
//		gonz_state.currentSlip = (ep->ActualRPM(0)-ep->ActualRPM(1)+ep->ActualRPM(2)-ep->ActualRPM(3));		
	}
	if (current_settings.rotationControlEnabled > 0) {
		double setRot = current_settings.rotationControlP*gonz_state.currentRotationError + current_settings.rotationControlI*gonz_state.rotationErrorInt +
				current_settings.rotationControlD*(gonz_state.currentRotationError - gonz_state.lastRotationError);


		double velo = sqrt(gonz_state.currentMotionGoal.x*gonz_state.currentMotionGoal.x+gonz_state.currentMotionGoal.y*gonz_state.currentMotionGoal.y);
		setRot += current_settings.rotationControlByVeloP*gonz_state.currentRotationError*velo;


		setRot = setRot*current_settings.robotRadius*finfactor/1024.0;
	        for (i=0; i < 4; i++) {
			gonz_state.currentMotorGoal[i] += setRot;
		}					
	}
    //printf("RPM: %f\n",gonz_state.currentMotorGoal[0]);
    gonz_send_cmd();
}
void gonz_calc_odometry() { //TODO: Optimise!
    unsigned char i;
    gonz_state.actualMotion.rotation = 0;
    for (i=0; i<4; i++) {
        gonz_state.actualMotion.rotation += ep->ActualRPM(i);
    }
    gonz_state.actualMotion.rotation *= (0.25 * 1024.0  * wheelcirc * current_settings.gear_ratio_nominator/ (current_settings.gear_ratio_denominator * current_settings.robotRadius * 60.0));
    gonz_state.actualMotion.x =  -ep->ActualRPM(0);
    gonz_state.actualMotion.x += -ep->ActualRPM(1);
    gonz_state.actualMotion.x +=  ep->ActualRPM(2);
    gonz_state.actualMotion.x +=  ep->ActualRPM(3);
    gonz_state.actualMotion.x *= ((double)current_settings.gear_ratio_nominator) / ((double)current_settings.gear_ratio_denominator * cosphi*4.0*60.0)*wheelcirc;

    gonz_state.actualMotion.y =   ep->ActualRPM(0);
    gonz_state.actualMotion.y += -ep->ActualRPM(1);
    gonz_state.actualMotion.y += -ep->ActualRPM(2);
    gonz_state.actualMotion.y +=  ep->ActualRPM(3);
    gonz_state.actualMotion.y *= (double)current_settings.gear_ratio_nominator / ((double)current_settings.gear_ratio_denominator*sinphi*4.0*60.0)*wheelcirc;

    //printf("ODO: x: %f\ty: %f\tr: %f\trpm:%d\n",gonz_state.actualMotion.x,gonz_state.actualMotion.y,gonz_state.actualMotion.rotation,ep->ActualRPM(0));

	gonz_state.currentSlip = (ep->ActualRPM(0)-ep->ActualRPM(1)+ep->ActualRPM(2)-ep->ActualRPM(3));
	gonz_state.currentSlip /= 4.0;
	gonz_state.slipI *= fabs(1.0-current_settings.slipControlDecay);
	gonz_state.slipI += gonz_state.currentSlip;

	gonz_state.lastRotationError = gonz_state.currentRotationError;
	gonz_state.currentRotationError = gonz_state.currentMotionGoal.rotation-gonz_state.actualMotion.rotation;
	gonz_state.rotationErrorInt += gonz_state.currentRotationError;
	gonz_state.rotationErrorInt = CLAMP(gonz_state.rotationErrorInt,current_settings.maxRotationErrorInt);
	//gonz_state.currentSlip *= ((double)current_settings.gear_ratio_nominator) / ((double)current_settings.gear_ratio_denominator * 4.0);


    gettimeofday(&odotime_cur, NULL);
    unsigned long long timediff = TIMEDIFFMS(odotime_cur,odotime_last);//(odotime_cur.tv_sec*1000+odotime_cur.tv_usec/1000)-(odotime_last.tv_sec*1000+odotime_last.tv_usec/1000);
    //printf("time: %llu\n",timediff);
    //Position update:
    double angle = atan2(gonz_state.actualMotion.y,gonz_state.actualMotion.x);
    double trans = sqrt(gonz_state.actualMotion.y*gonz_state.actualMotion.y+gonz_state.actualMotion.x*gonz_state.actualMotion.x)*(double)timediff/1000.0;
    double rot = gonz_state.actualMotion.rotation/1024.0*(double)timediff/1000.0;
    double xtemp,ytemp;

    if (rot!=0) {
        double radius = trans / rot;
        xtemp = abs(sin(rot) * radius) * SIGN(trans);
		ytemp = (radius - (cos(rot)) * radius) * SIGN(rot);
    }
    else {
        xtemp = trans;
        ytemp = 0;
    }

    double h = gonz_state.currentPosition.angle + angle;
    double cos_h = cos(h);
	double sin_h = sin(h);
	double xtemp1 = cos_h*xtemp - sin_h*ytemp;
	double ytemp1 = sin_h*xtemp + cos_h*ytemp;

    gonz_state.currentPosition.angle += rot;

    if (gonz_state.currentPosition.angle > PI) {
        gonz_state.currentPosition.angle -= TWO_PI;
    } else if (gonz_state.currentPosition.angle < -PI) {
        gonz_state.currentPosition.angle += TWO_PI;
    }
    gonz_state.currentPosition.x += xtemp1;
    gonz_state.currentPosition.y += ytemp1;

    odotime_last = odotime_cur;

    long long tosend = (long long)TIMEDIFFMS(odotime_cur,lastOdoSendTime) - (long long)current_settings.odometrySamplingTime;

    if (tosend >= 0) {
		if (gonz_state.newOdometryAvailable > 0) {
			gonz_state.newOdometryAvailable = 0; //there is a race condition here, but hardly worth the effort of locking
			gonz_send_odometry();
			lastOdoSendTime = odotime_cur;
		}
    }

}
void gonz_send_odometry() {
    RawOdometryInfo ro;
    MotionInfo mi;
    mi.angle = atan2(gonz_state.actualMotion.y,gonz_state.actualMotion.x);
    mi.translation= sqrt(gonz_state.actualMotion.y*gonz_state.actualMotion.y+gonz_state.actualMotion.x*gonz_state.actualMotion.x);
    mi.rotation = gonz_state.actualMotion.rotation/1024.0;

    ro.motion = mi;
	unsigned long long sendtime = supplementary::DateTime::getUtcNowC()-(current_settings.odometrySamplingTime*10000l)/2;
   // unsigned long long sendtime = (odotime_cur.tv_sec*1000000+odotime_cur.tv_usec)*10 -(current_settings.odometrySamplingTime*10000l)/2;
    ro.timestamp = sendtime;

    PositionInfo pi;
    pi.angle = gonz_state.currentPosition.angle;
    pi.x = gonz_state.currentPosition.x;
    pi.y = gonz_state.currentPosition.y;

    ro.position = pi;
    RosHelper::rawOdo = ro;
    RosHelper::sendOdometry();
//std::cout << pi->getAngle() << "\t" << pi->getX() << "\t" << pi->getY() << "\t" << ro->getTimestamp() <<"\n";
}
void gonz_send_cmd() {
	unsigned char i;
	for(i=0; i<CONTROLLER_COUNT; i++) {
		ep->SetDemandRPM(i,gonz_state.currentMotorGoal[i]);
	}
}

int gonz_check_status() {    
    if(ep->IsFunctional() < 0) {
		gonz_set_error_state(1,GONZ_REST_ON_ERROR_MS);
	return -1;
    }
    return 1;
}

void gonz_idle() {
	if (gonz_state.state == GONZ_ERROR) {

                if (gonz_state.autorecover) { //if not set, reboot is necessary
//printf("auto recover!\n");
			ep->Trigger(-1);
			ep->ResetFaults();
               		long timediff =TIMEDIFFMS(gonz_cur_calltime,gonz_last_calltime);
                    	gonz_state.recover_timer -= timediff;
                    //printf("Timediff: %ld timer: %d\n",timediff,gonz_state.recover_timer);
              		if (gonz_state.recover_timer <= 0) {
      				printf("Recovering from error...\n");
                     		gonz_recover_from_error();
                    	}
                }
	} else {
		gonz_calc_odometry();
		gonz_reset();
		gonz_check_status();
		ep->Trigger(0);
	}
}


void gonz_set_error_state(int mayrecover,int timetorecover) {
    if (gonz_state.state == GONZ_ERROR) {
        if (!mayrecover) {
            gonz_state.autorecover=0;
        } else {
            if (timetorecover > gonz_state.recover_timer) {
                gonz_state.recover_timer = timetorecover;
            }
        }
    }
    else {
        gonz_state.state=GONZ_ERROR;
        gonz_state.autorecover = mayrecover;
        gonz_state.recover_timer = timetorecover;
    }
}
void gonz_emergency_stop() {
	ep->EmergencyShutdown();
    //TODO
}
void gonz_recover_from_error() {
    printf("Recovering...\n");
    gonz_state.recover_timer = GONZ_REST_ON_ERROR_MS;
    if (ep->InitAllNodes()>0) {
         gonz_state.state=GONZ_RUNNING;
    }
    gonz_reset();

}
void gonz_reset() {
	gonz_state.currentMotionGoal.rotation = 0;
	gonz_state.currentMotionGoal.x = 0;
	gonz_state.currentMotionGoal.y = 0;
	gonz_state.currentMotorGoal[0] = 0;
	gonz_state.currentMotorGoal[1] = 0;
	gonz_state.currentMotorGoal[2] = 0;
	gonz_state.currentMotorGoal[3] = 0;
	gonz_state.currentSlip = 0;
	gonz_state.slipI = 0;
	gonz_state.currentRotationError = 0;
	gonz_state.lastRotationError = 0;
	gonz_state.rotationErrorInt = 0;
	//gonz_state.newOdometryAvailable = 0;
    	gettimeofday(&odotime_last, NULL);
    	gettimeofday(&odotime_cur, NULL);
}

void gonz_set_motion_request(double angle, double trans, double rot) {
	gonz_state.currentMotionGoal.rotation=(
		MIN(current_settings.maxRotation,fabs(rot))*((double)SIGN2(rot)*1024.0)
	);
	trans = ((double)SIGN2(trans))*MIN(current_settings.maxTranslation,fabs(trans));
	gonz_state.currentMotionGoal.x=(trans*cos(angle));
	gonz_state.currentMotionGoal.y=(trans*sin(angle));

}
/*
Wheel Layout:

	1 phi 4
	\    /
	 \  /
y <_______\/____  180-phi
	  /\
	 /  \
	/    \
	2     3
phi = 50

all directions counter clockwise

Matrix Stuff:

v1	    (-a +b 1)(vx)
v2 = 	(-a -b 1)(vy)
v3	    (+a -b 1)(rW)
v4	    (+a +b 1)

where a = cos(phi) and b = sin(phi)

vx		        (-1/a -1/a +1/a +1/a) (v1)
vy = 	 1/4	(+1/b -1/b -1/b +1/b) (v2)
rW		        ( 1    1    1    1)   (v3)
                                      (v4)

Bad Motion Vector B=(1,-1,1,-1)^T
Odometry Correction:

	V = V - (v1-v2+v3-v4)/4*B

	(v1-v2+v3-v4)/4 corresponds to the magnitude of the deviation
*/
void gonz_test_loop() {
	char charbuf[256];
	unsigned int nodeid;
	int value;
	if(fgets(charbuf,255, stdin)!=NULL) {
	    printf("Read: %s\n",charbuf);
	    if(strcmp("st\n",charbuf)==0) {
	       ep->PrintStatus();
	    } else if(strcmp("e all\n",charbuf)==0) {
		printf("Enabling all drives!\n");
		ep->EnableAllNodes();
	    }
	    else if(strcmp("d all\n",charbuf)==0) {
		printf("Disabling all drives!\n");
		ep->DisableAllNodes();
	    }
	    else if (sscanf(charbuf,"e %ud",&nodeid)>0) {

	//printf("%d\n",nodeid);
		    if (nodeid <= CONTROLLER_COUNT && nodeid > 0) {
		        printf("Enabling Node %d\n",nodeid);
		        ep->EnableNode(nodeid);
		    }

	    }
	    else if (sscanf(charbuf,"d %ud",&nodeid)>0) {
		    if (nodeid <= CONTROLLER_COUNT && nodeid > 0) {
		        printf("Disabling Node %d\n",nodeid);
		        ep->DisableNode(nodeid);
		    }

	    }
	    else if (sscanf(charbuf,"%1ud",&nodeid)>0 && sscanf(charbuf+1,"v %d",&value)>0) {
		    if (nodeid <= CONTROLLER_COUNT && nodeid > 0 && value > -10000 && value < 10000) {
		        printf("Setting velocity for node %d to %d\n",nodeid,value);
			ep->SetVelocityDirect(nodeid,value);
		        //mcdc_set_velocity_direct(nodeid,value);
		    }

	    }
	}
	ep->Trigger(-1);
	//printf("%d\t%d\t%d\t%d\n",mcdc[0].status.actualRPM,mcdc[1].status.actualRPM,mcdc[2].status.actualRPM,mcdc[3].status.actualRPM);
}


void gonz_notify_odometry() {
	gonz_state.newOdometryAvailable = 1;

}
