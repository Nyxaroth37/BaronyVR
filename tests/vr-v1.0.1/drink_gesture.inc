struct Gesture {
    Uint32 uid=0,owner=0,last=0,queuedAt=0;
    int type=-1,count=0,level=-1;bool secret=false;
    bool armed=false,latched=false,pending=false;
    void reset(){*this=Gesture{};}
    bool sample(Uint32 now,Uint32 item,Uint32 actor,int itemType,int itemCount,
        int mapLevel,bool mapSecret,float distance,float grip,float trigger) {
        if(uid!=item || owner!=actor || type!=itemType || level!=mapLevel || secret!=mapSecret) {
            reset();uid=item;owner=actor;type=itemType;level=mapLevel;secret=mapSecret;
        }
        if(last && now-last>150u){armed=false;pending=false;}
        last=now;
        if(!std::isfinite(distance)||!std::isfinite(grip)||!std::isfinite(trigger)) {
            reset();return false;
        }
        if(pending && now-queuedAt>150u)pending=false;
        // Observe a released grip even at the face. A grip already held while
        // approaching cannot drink; the press itself must occur in the zone.
        if(grip<.35f){armed=true;latched=false;return false;}
        if(grip<.65f || !armed)return false;
        armed=false;latched=true; // consume every press, including out of range
        if(distance>.25f || trigger>=.15f || pending)return false;
        pending=true;queuedAt=now;count=itemCount;
        return true;
    }
};
